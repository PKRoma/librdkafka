/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @brief OAUTHBEARER AWS plugin entry point (M4).
 *
 * Installs a refresh callback via
 * rd_kafka_conf_set_oauthbearer_token_refresh_cb() at plugin-load
 * time, plus the four M2 interceptors (on_conf_set, on_new,
 * on_conf_dup, on_conf_destroy). When librdkafka fires the refresh
 * callback, the plugin looks up the client's plugin-ctx via a
 * static rk->ctx registry, calls the STS provider to mint a JWT, and
 * hands it to rd_kafka_oauthbearer_set_token() / _set_token_failure().
 *
 * Why a registry: librdkafka's refresh-callback API passes the conf
 * opaque (set via rd_kafka_conf_set_opaque) as the callback's third
 * argument. That opaque belongs to the user, not us — we must not
 * steal it. The static rk->ctx map keeps plugin state off the user's
 * opaque slot.
 *
 * Next milestone:
 *   M5 — formalise Aws::InitAPI lifecycle doc + thread-safety reasoning.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdkafka.h"

#include "aws_sts_provider.h"
#include "config.h"
#include "rdkafka_oauthbearer_aws.h"

/* Export attribute that survives the plugin-wide -fvisibility=hidden
 * default applied in CMakeLists.txt (M6). Without the explicit default
 * visibility on non-Windows, dlsym() would fail to resolve conf_init
 * and the plugin loader would report "symbol not found". */
#ifdef _WIN32
#define RD_OAUTHBEARER_AWS_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RD_OAUTHBEARER_AWS_EXPORT __attribute__((visibility("default")))
#else
#define RD_OAUTHBEARER_AWS_EXPORT
#endif

static const char PLUGIN_NAME[] = "rdkafka-oauthbearer-aws";

/* ------------------------------------------------------------------------- */
/* rk -> ctx registry                                                        */
/*                                                                           */
/* Protects the refresh callback from racing with on_new inserts / on_conf   */
/* _destroy removes. Simple linked list because client counts are small      */
/* in practice; can be swapped for a hash map later if profiling warrants.   */
/* ------------------------------------------------------------------------- */

struct oba_registry_entry {
        rd_kafka_t *rk;
        rd_kafka_oauthbearer_aws_conf_t *ctx;
        struct oba_registry_entry *next;
};

static pthread_mutex_t g_registry_mu       = PTHREAD_MUTEX_INITIALIZER;
static struct oba_registry_entry *g_registry = NULL;

static void oba_registry_insert(rd_kafka_t *rk,
                                rd_kafka_oauthbearer_aws_conf_t *ctx) {
        struct oba_registry_entry *e =
            (struct oba_registry_entry *)calloc(1, sizeof(*e));
        if (!e)
                return;  /* best-effort; refresh cb will error out */
        e->rk  = rk;
        e->ctx = ctx;
        pthread_mutex_lock(&g_registry_mu);
        e->next    = g_registry;
        g_registry = e;
        pthread_mutex_unlock(&g_registry_mu);
}

static void oba_registry_remove(rd_kafka_t *rk) {
        struct oba_registry_entry **pp;
        pthread_mutex_lock(&g_registry_mu);
        for (pp = &g_registry; *pp; pp = &(*pp)->next) {
                if ((*pp)->rk == rk) {
                        struct oba_registry_entry *e = *pp;
                        *pp                          = e->next;
                        free(e);
                        break;
                }
        }
        pthread_mutex_unlock(&g_registry_mu);
}

static rd_kafka_oauthbearer_aws_conf_t *oba_registry_find(rd_kafka_t *rk) {
        struct oba_registry_entry *e;
        rd_kafka_oauthbearer_aws_conf_t *result = NULL;
        pthread_mutex_lock(&g_registry_mu);
        for (e = g_registry; e; e = e->next) {
                if (e->rk == rk) {
                        result = e->ctx;
                        break;
                }
        }
        pthread_mutex_unlock(&g_registry_mu);
        return result;
}

/* ------------------------------------------------------------------------- */
/* OAUTHBEARER refresh callback                                              */
/* ------------------------------------------------------------------------- */

static void oba_refresh_cb(rd_kafka_t *rk,
                           const char *oauthbearer_config,
                           void *opaque) {
        rd_kafka_oauthbearer_aws_conf_t *ctx;
        char *token     = NULL;
        int64_t exp_ms  = 0;
        char errstr[512];
        char setstr[512];

        /* oauthbearer_config is the value of the sasl.oauthbearer.config
         * property — this plugin uses its own sasl.oauthbearer.aws.* namespace
         * instead, so it's ignored here. opaque is the user's conf opaque
         * (see rd_kafka_conf_set_opaque); we also don't use it because we
         * must not reserve that slot for plugin state. */
        (void)oauthbearer_config;
        (void)opaque;

        ctx = oba_registry_find(rk);
        if (!ctx) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "rdkafka-oauthbearer-aws: no plugin context registered "
                    "for this client. Did plugin.library.paths load before "
                    "rd_kafka_new?");
                return;
        }

        if (ctx->provider_error) {
                rd_kafka_oauthbearer_set_token_failure(rk,
                                                       ctx->provider_error);
                return;
        }

        if (!ctx->provider) {
                rd_kafka_oauthbearer_set_token_failure(
                    rk,
                    "rdkafka-oauthbearer-aws: internal error: STS provider "
                    "not constructed");
                return;
        }

        if (rd_kafka_aws_sts_provider_get_token(ctx->provider, &token,
                                                &exp_ms, errstr,
                                                sizeof(errstr)) != 0) {
                rd_kafka_oauthbearer_set_token_failure(rk, errstr);
                return;
        }

        /* Principal name: librdkafka requires something non-empty. The
         * authoritative identity is carried in the JWT's `sub` claim; the
         * OAUTHBEARER principal name is opaque to the broker for AWS STS
         * flows. Use a fixed identifier; M-later could parse it out of
         * the JWT if a use case appears. */
        if (rd_kafka_oauthbearer_set_token(rk, token, exp_ms,
                                           "aws-sts-web-identity",
                                           NULL, 0, setstr,
                                           sizeof(setstr)) !=
            RD_KAFKA_RESP_ERR_NO_ERROR) {
                rd_kafka_oauthbearer_set_token_failure(rk, setstr);
        }

        free(token);
}

/* ------------------------------------------------------------------------- */
/* Interceptor callbacks                                                     */
/* ------------------------------------------------------------------------- */

static rd_kafka_resp_err_t
register_interceptors(rd_kafka_conf_t *conf,
                      rd_kafka_oauthbearer_aws_conf_t *ctx,
                      char *errstr,
                      size_t errstr_size);

static rd_kafka_conf_res_t oba_on_conf_set(rd_kafka_conf_t *conf,
                                           const char *name,
                                           const char *val,
                                           char *errstr,
                                           size_t errstr_size,
                                           void *ic_opaque) {
        (void)conf;
        return rd_kafka_oauthbearer_aws_conf_set(
            (rd_kafka_oauthbearer_aws_conf_t *)ic_opaque, name, val, errstr,
            errstr_size);
}

static rd_kafka_resp_err_t oba_on_new(rd_kafka_t *rk,
                                      const rd_kafka_conf_t *conf,
                                      void *ic_opaque,
                                      char *errstr,
                                      size_t errstr_size) {
        rd_kafka_oauthbearer_aws_conf_t *ctx;
        char local_err[512];

        (void)conf;

        ctx     = (rd_kafka_oauthbearer_aws_conf_t *)ic_opaque;
        ctx->rk = rk;

        /* Validate + construct provider. On failure we still register in
         * the registry and store the error message, so the refresh
         * callback can surface a specific error rather than a generic
         * "no plugin context" one. */
        if (rd_kafka_oauthbearer_aws_conf_validate(
                ctx, local_err, sizeof(local_err)) != 0) {
                ctx->provider_error = strdup(local_err);
                snprintf(errstr, errstr_size, "%s", local_err);
                oba_registry_insert(rk, ctx);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        ctx->provider = rd_kafka_aws_sts_provider_new(ctx, local_err,
                                                      sizeof(local_err));
        if (!ctx->provider) {
                ctx->provider_error = strdup(local_err);
                snprintf(errstr, errstr_size, "%s", local_err);
                oba_registry_insert(rk, ctx);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        oba_registry_insert(rk, ctx);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

static rd_kafka_resp_err_t oba_on_conf_destroy(void *ic_opaque) {
        rd_kafka_oauthbearer_aws_conf_t *ctx =
            (rd_kafka_oauthbearer_aws_conf_t *)ic_opaque;
        /* If on_new ever fired, our ctx is in the registry keyed by rk.
         * Remove before the ctx (and its provider) are freed so a
         * late refresh callback can't find a stale entry. */
        if (ctx->rk)
                oba_registry_remove(ctx->rk);
        rd_kafka_oauthbearer_aws_conf_destroy(ctx);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

static rd_kafka_resp_err_t
oba_on_conf_dup(rd_kafka_conf_t *new_conf,
                const rd_kafka_conf_t *old_conf,
                size_t filter_cnt,
                const char **filter,
                void *ic_opaque) {
        rd_kafka_oauthbearer_aws_conf_t *new_ctx;
        char errstr[512];

        (void)old_conf;
        (void)filter_cnt;
        (void)filter;
        (void)ic_opaque;

        new_ctx = rd_kafka_oauthbearer_aws_conf_new();
        if (!new_ctx)
                return RD_KAFKA_RESP_ERR__FAIL;

        /* Re-register interceptors on the duplicated conf. librdkafka
         * replays the parent conf's tracked sasl.oauthbearer.aws.*
         * property values through the new on_conf_set, repopulating the
         * fresh ctx. The refresh callback on the new conf is set
         * fresh below as well. */
        rd_kafka_conf_set_oauthbearer_token_refresh_cb(new_conf,
                                                       oba_refresh_cb);
        if (register_interceptors(new_conf, new_ctx, errstr, sizeof(errstr)) !=
            RD_KAFKA_RESP_ERR_NO_ERROR) {
                rd_kafka_oauthbearer_aws_conf_destroy(new_ctx);
                return RD_KAFKA_RESP_ERR__FAIL;
        }
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/* ------------------------------------------------------------------------- */
/* Interceptor registration                                                  */
/* ------------------------------------------------------------------------- */

static rd_kafka_resp_err_t
register_interceptors(rd_kafka_conf_t *conf,
                      rd_kafka_oauthbearer_aws_conf_t *ctx,
                      char *errstr,
                      size_t errstr_size) {
        rd_kafka_resp_err_t err;

        err = rd_kafka_conf_interceptor_add_on_conf_set(
            conf, PLUGIN_NAME, oba_on_conf_set, ctx);
        if (err)
                goto fail;

        err = rd_kafka_conf_interceptor_add_on_new(conf, PLUGIN_NAME,
                                                   oba_on_new, ctx);
        if (err)
                goto fail;

        err = rd_kafka_conf_interceptor_add_on_conf_dup(
            conf, PLUGIN_NAME, oba_on_conf_dup, ctx);
        if (err)
                goto fail;

        err = rd_kafka_conf_interceptor_add_on_conf_destroy(
            conf, PLUGIN_NAME, oba_on_conf_destroy, ctx);
        if (err)
                goto fail;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

fail:
        snprintf(errstr, errstr_size,
                 "%s: failed to register interceptor: %s", PLUGIN_NAME,
                 rd_kafka_err2str(err));
        return err;
}

/* ------------------------------------------------------------------------- */
/* Plugin entry points                                                        */
/* ------------------------------------------------------------------------- */

static rd_kafka_resp_err_t install_plugin(rd_kafka_conf_t *conf,
                                          char *errstr,
                                          size_t errstr_size) {
        rd_kafka_oauthbearer_aws_conf_t *ctx;
        rd_kafka_resp_err_t err;

        ctx = rd_kafka_oauthbearer_aws_conf_new();
        if (!ctx) {
                snprintf(errstr, errstr_size,
                         "%s: out of memory allocating plugin context",
                         PLUGIN_NAME);
                return RD_KAFKA_RESP_ERR__FAIL;
        }

        /* Set the refresh callback FIRST so that if the user later calls
         * rd_kafka_conf_set_opaque() it doesn't affect us (we don't use the
         * opaque), and so that the callback is in place before any
         * client creation. */
        rd_kafka_conf_set_oauthbearer_token_refresh_cb(conf, oba_refresh_cb);

        err = register_interceptors(conf, ctx, errstr, errstr_size);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
                rd_kafka_oauthbearer_aws_conf_destroy(ctx);
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

RD_OAUTHBEARER_AWS_EXPORT
rd_kafka_resp_err_t conf_init(rd_kafka_conf_t *conf,
                              void **plug_opaquep,
                              char *errstr,
                              size_t errstr_size) {
        rd_kafka_resp_err_t err = install_plugin(conf, errstr, errstr_size);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
                return err;

        /* The plugin's opaque slot (distinct from the user's conf opaque)
         * is unused today. */
        *plug_opaquep = NULL;

        fprintf(stderr,
                "%s: plugin loaded (M4: refresh callback + interceptors)\n",
                PLUGIN_NAME);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

RD_OAUTHBEARER_AWS_EXPORT
rd_kafka_resp_err_t
rd_kafka_oauthbearer_aws_register(rd_kafka_conf_t *conf,
                                  char *errstr,
                                  size_t errstr_size) {
        return install_plugin(conf, errstr, errstr_size);
}
