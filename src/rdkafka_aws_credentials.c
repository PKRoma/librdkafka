/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2026, Confluent Inc.
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
 * @file rdkafka_aws_credentials.c
 * @brief AWS credentials types + provider chain (Milestone 1 of V1).
 *        See DESIGN_AWS_OAUTHBEARER_V1.md at the repo root.
 *
 * This milestone only implements the in-memory pieces: credentials struct,
 * environment-variable provider, and chain dispatcher. Network-backed
 * providers (IMDS, ECS, web identity) and the cached wrapper arrive in
 * subsequent milestones.
 */

#include "rdkafka_int.h"
#include "rdkafka_aws_credentials.h"
#include "rdunittest.h"

#if WITH_CURL
#define CJSON_HIDE_SYMBOLS
#include "cJSON.h"
#include "rdhttp.h"
#include "rdkafka_aws_sts.h"
#endif


/*
 * ============================================================
 * Credentials struct
 * ============================================================
 */

rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_new(const char *access_key_id,
                             const char *secret_access_key,
                             const char *session_token,
                             rd_ts_t expiration_us) {
        rd_kafka_aws_credentials_t *c;

        rd_assert(access_key_id && *access_key_id);
        rd_assert(secret_access_key && *secret_access_key);

        c                    = rd_calloc(1, sizeof(*c));
        c->access_key_id     = rd_strdup(access_key_id);
        c->secret_access_key = rd_strdup(secret_access_key);
        c->session_token =
            (session_token && *session_token) ? rd_strdup(session_token) : NULL;
        c->expiration_us = expiration_us;
        rd_refcnt_init(&c->refcnt, 1);
        return c;
}

rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_keep(rd_kafka_aws_credentials_t *creds) {
        rd_refcnt_add(&creds->refcnt);
        return creds;
}

static void
rd_kafka_aws_credentials_destroy_final(rd_kafka_aws_credentials_t *c) {
        if (c->access_key_id) {
                /* Best-effort scrub of secret material before free.
                 * Not a substitute for proper memory hygiene at the syscall
                 * boundary, but reduces exposure in core dumps. */
                memset(c->access_key_id, 0, strlen(c->access_key_id));
                rd_free(c->access_key_id);
        }
        if (c->secret_access_key) {
                memset(c->secret_access_key, 0, strlen(c->secret_access_key));
                rd_free(c->secret_access_key);
        }
        if (c->session_token) {
                memset(c->session_token, 0, strlen(c->session_token));
                rd_free(c->session_token);
        }
        rd_refcnt_destroy(&c->refcnt);
        rd_free(c);
}

void rd_kafka_aws_credentials_destroy(rd_kafka_aws_credentials_t *creds) {
        if (!creds)
                return;
        rd_refcnt_destroywrapper(&creds->refcnt,
                                 rd_kafka_aws_credentials_destroy_final(creds));
}

rd_bool_t
rd_kafka_aws_credentials_expired(const rd_kafka_aws_credentials_t *creds,
                                 rd_ts_t now_us) {
        if (creds->expiration_us == 0)
                return rd_false;
        return now_us >= creds->expiration_us;
}


/*
 * ============================================================
 * Generic provider helpers
 * ============================================================
 */

void rd_kafka_aws_creds_provider_destroy(rd_kafka_aws_creds_provider_t *p) {
        if (!p)
                return;
        if (p->destroy)
                p->destroy(p);
        else
                rd_free(p);
}

rd_kafka_aws_creds_result_t
rd_kafka_aws_creds_provider_resolve(rd_kafka_aws_creds_provider_t *p,
                                    rd_kafka_aws_credentials_t **credsp,
                                    char *errstr,
                                    size_t errstr_size) {
        rd_assert(p && p->resolve);
        *credsp = NULL;
        if (errstr_size > 0)
                errstr[0] = '\0';
        return p->resolve(p, credsp, errstr, errstr_size);
}


/*
 * ============================================================
 * Environment-variable provider
 * ============================================================
 */

static rd_kafka_aws_creds_result_t
env_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                     rd_kafka_aws_credentials_t **credsp,
                     char *errstr,
                     size_t errstr_size) {
        const char *akid   = rd_getenv("AWS_ACCESS_KEY_ID", NULL);
        const char *secret = rd_getenv("AWS_SECRET_ACCESS_KEY", NULL);
        const char *token  = rd_getenv("AWS_SESSION_TOKEN", NULL);

        /* Treat empty-string env var as unset; some container runtimes
         * set the variable to "" when they mean "don't use". */
        if (akid && !*akid)
                akid = NULL;
        if (secret && !*secret)
                secret = NULL;
        if (token && !*token)
                token = NULL;

        if (!akid && !secret)
                return RD_KAFKA_AWS_CREDS_SKIP;

        if (!akid || !secret) {
                rd_snprintf(errstr, errstr_size,
                            "AWS credentials in environment are incomplete: "
                            "both AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY "
                            "must be set");
                return RD_KAFKA_AWS_CREDS_FATAL;
        }

        *credsp = rd_kafka_aws_credentials_new(akid, secret, token, 0);
        return RD_KAFKA_AWS_CREDS_OK;
}

static void env_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        rd_free(self);
}

rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_env_new(rd_kafka_t *rk) {
        rd_kafka_aws_creds_provider_t *p;

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "env";
        p->resolve = env_provider_resolve;
        p->destroy = env_provider_destroy;
        p->rk      = rk;
        return p;
}


/*
 * ============================================================
 * Chain provider
 * ============================================================
 */

typedef struct chain_opaque_s {
        rd_kafka_aws_creds_provider_t **providers;
        size_t n_providers;
} chain_opaque_t;

static rd_kafka_aws_creds_result_t
chain_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                       rd_kafka_aws_credentials_t **credsp,
                       char *errstr,
                       size_t errstr_size) {
        chain_opaque_t *o = self->opaque;
        size_t i;

        for (i = 0; i < o->n_providers; i++) {
                rd_kafka_aws_creds_provider_t *inner = o->providers[i];
                rd_kafka_aws_creds_result_t r;

                r = rd_kafka_aws_creds_provider_resolve(inner, credsp, errstr,
                                                        errstr_size);

                if (r == RD_KAFKA_AWS_CREDS_OK)
                        return RD_KAFKA_AWS_CREDS_OK;
                if (r == RD_KAFKA_AWS_CREDS_FATAL) {
                        /* Prepend provider name for context. If errstr is empty
                         * (provider didn't populate it), at least name it. */
                        if (errstr_size > 0) {
                                char tmp[512];
                                rd_snprintf(tmp, sizeof(tmp),
                                            "aws credentials provider \"%s\": "
                                            "%s",
                                            inner->name,
                                            errstr[0] ? errstr : "failed");
                                rd_snprintf(errstr, errstr_size, "%s", tmp);
                        }
                        return RD_KAFKA_AWS_CREDS_FATAL;
                }
                /* SKIP: continue to next */
        }

        /* All providers skipped. Surface as SKIP so the caller (typically a
         * cached wrapper or SASL refresh path) can decide whether that's a
         * hard error in its own context. */
        return RD_KAFKA_AWS_CREDS_SKIP;
}

static void chain_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        chain_opaque_t *o = self->opaque;
        size_t i;

        for (i = 0; i < o->n_providers; i++)
                rd_kafka_aws_creds_provider_destroy(o->providers[i]);
        rd_free(o->providers);
        rd_free(o);
        rd_free(self);
}

rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_chain_new(rd_kafka_t *rk,
                                      rd_kafka_aws_creds_provider_t **providers,
                                      size_t n_providers) {
        rd_kafka_aws_creds_provider_t *p;
        chain_opaque_t *o;

        rd_assert(providers && n_providers > 0);

        o              = rd_calloc(1, sizeof(*o));
        o->providers   = rd_malloc(sizeof(*o->providers) * n_providers);
        o->n_providers = n_providers;
        memcpy(o->providers, providers, sizeof(*o->providers) * n_providers);

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "chain";
        p->resolve = chain_provider_resolve;
        p->destroy = chain_provider_destroy;
        p->rk      = rk;
        p->opaque  = o;
        return p;
}


#if WITH_CURL

/*
 * ============================================================
 * ISO 8601 / RFC 3339 timestamp parser
 * ============================================================
 *
 * Accepts the two formats observed in AWS responses (verified 2026-04-21
 * against real STS + IMDSv2):
 *   - "2026-04-21T11:15:00Z"             (classic STS / IMDSv2)
 *   - "2026-04-21T06:06:47.641000+00:00" (GetWebIdentityToken)
 *
 * Fractional seconds are parsed and discarded (we only care about expiry
 * with ~seconds precision). Timezone suffixes: Z, +HH:MM, -HH:MM, +HHMM.
 */

/**
 * @brief Convert (YYYY, MM, DD, hh, mm, ss) UTC to Unix epoch seconds.
 *
 * Uses a portable Julian-Day-style formula so we don't depend on the
 * non-portable timegm()/_mkgmtime() pair. Valid for years 1970..9999.
 */
static int64_t rd_aws_ymd_hms_to_epoch(int year,
                                       int mon,
                                       int day,
                                       int hour,
                                       int min,
                                       int sec) {
        int64_t y, m, era, yoe, doy, doe, days;

        /* Howard Hinnant's days_from_civil (public domain). */
        y    = (int64_t)year - (mon <= 2 ? 1 : 0);
        m    = mon;
        era  = (y >= 0 ? y : y - 399) / 400;
        yoe  = y - era * 400;                                    /* [0,399] */
        doy  = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1; /* [0,365] */
        doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* [0,146096] */
        days = era * 146097 + doe -
               719468; /* 719468 = days_from_civil(1970,3,1) */

        return days * 86400LL + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

/**
 * @brief Parse an ISO 8601 / RFC 3339 timestamp string to UTC epoch seconds.
 *
 * @returns rd_true on success with \p *epoch_seconds set, else rd_false.
 */
rd_bool_t rd_aws_parse_iso8601_utc(const char *iso, int64_t *epoch_seconds) {
        int year, mon, day, hour, min, sec;
        int tz_hour = 0, tz_min = 0, tz_sign = 1;
        int n = 0;
        int64_t epoch;

        if (!iso)
                return rd_false;

        if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &mon, &day, &hour,
                   &min, &sec, &n) != 6)
                return rd_false;

        /* Skip optional fractional seconds: .fff... */
        if (iso[n] == '.') {
                n++;
                while (iso[n] >= '0' && iso[n] <= '9')
                        n++;
        }

        /* Timezone: Z, +HH:MM, -HH:MM, +HHMM — required.
         * AWS always includes one; we reject ambiguous local-time strings. */
        if (iso[n] == 'Z' || iso[n] == 'z') {
                n++;
        } else if (iso[n] == '+' || iso[n] == '-') {
                tz_sign = (iso[n] == '+') ? 1 : -1;
                n++;
                if (sscanf(iso + n, "%2d:%2d", &tz_hour, &tz_min) == 2) {
                        n += 5; /* HH:MM */
                } else if (sscanf(iso + n, "%2d%2d", &tz_hour, &tz_min) == 2) {
                        n += 4; /* HHMM */
                } else {
                        return rd_false;
                }
        } else {
                return rd_false;
        }

        /* Reject trailing junk after the tz designator. */
        if (iso[n] != '\0')
                return rd_false;

        /* Rough sanity bounds. */
        if (year < 1970 || year > 9999 || mon < 1 || mon > 12 || day < 1 ||
            day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 ||
            sec < 0 || sec > 60 || tz_hour < 0 || tz_hour > 23 || tz_min < 0 ||
            tz_min > 59)
                return rd_false;

        epoch = rd_aws_ymd_hms_to_epoch(year, mon, day, hour, min, sec);
        epoch -= (int64_t)tz_sign * (tz_hour * 3600 + tz_min * 60);

        *epoch_seconds = epoch;
        return rd_true;
}

/**
 * @brief Convert a wall-clock Unix-epoch-seconds timestamp to an
 *        rd_clock()-basis microsecond timestamp.
 *
 * Used to map AWS-provided absolute Expiration into our monotonic
 * credential expiration field. Clock-jump-safe because we compute the
 * delta from "now" at translation time.
 *
 * @returns 0 if \p exp_epoch is in the past, else the monotonic us value.
 */
rd_ts_t rd_aws_epoch_to_monotonic_us(int64_t exp_epoch) {
        int64_t now_epoch = (int64_t)time(NULL);
        int64_t seconds_until;

        if (exp_epoch <= now_epoch)
                return rd_clock(); /* treat as immediately expired */

        seconds_until = exp_epoch - now_epoch;
        return rd_clock() + seconds_until * 1000000LL;
}


/*
 * ============================================================
 * IMDSv2 provider
 * ============================================================
 */

#define IMDS_DEFAULT_ENDPOINT  "http://169.254.169.254"
#define IMDS_TOKEN_TTL_SECONDS 300
/* Link-local, should be fast. Keep tight so the chain moves on quickly
 * when not on EC2. */
#define IMDS_HTTP_TIMEOUT_S 2

typedef struct imds_opaque_s {
        char *endpoint; /* no trailing slash */
} imds_opaque_t;

/**
 * @brief Read response body from rd_buf_t into a freshly allocated,
 *        NUL-terminated C string. Caller owns the result.
 */
static char *rd_aws_buf_to_cstr(rd_buf_t *rbuf) {
        size_t len = rd_buf_len(rbuf);
        char *s    = rd_malloc(len + 1);
        rd_slice_t slice;
        rd_slice_init_full(&slice, rbuf);
        rd_slice_read(&slice, s, len);
        s[len] = '\0';
        return s;
}

/**
 * @brief Build a URL by concatenating \p base and \p path.
 *        Caller frees.
 */
static char *rd_aws_build_url(const char *base, const char *path) {
        size_t blen = strlen(base);
        size_t plen = strlen(path);
        char *url   = rd_malloc(blen + plen + 1);
        memcpy(url, base, blen);
        memcpy(url + blen, path, plen + 1);
        return url;
}

/**
 * @brief PUT /latest/api/token, return newly allocated session token string
 *        or NULL on error.
 */
static char *imds_fetch_session_token(rd_kafka_t *rk,
                                      const char *endpoint,
                                      rd_http_error_t **herrp) {
        char *url;
        rd_http_error_t *herr;
        rd_buf_t *rbuf = NULL;
        int code       = -1;
        char *token    = NULL;
        char ttl_header[64];
        char *headers[1];

        *herrp = NULL;

        rd_snprintf(ttl_header, sizeof(ttl_header),
                    "X-aws-ec2-metadata-token-ttl-seconds: %d",
                    IMDS_TOKEN_TTL_SECONDS);
        headers[0] = ttl_header;

        url  = rd_aws_build_url(endpoint, "/latest/api/token");
        herr = rd_http_put(rk, url, headers, 1, NULL, 0, IMDS_HTTP_TIMEOUT_S, 1,
                           100, &rbuf, NULL, &code);
        rd_free(url);

        if (herr) {
                *herrp = herr;
                return NULL;
        }
        if (code != 200) {
                *herrp = NULL; /* unusual but not a network error */
                RD_IF_FREE(rbuf, rd_buf_destroy_free);
                return NULL;
        }

        token = rd_aws_buf_to_cstr(rbuf);
        rd_buf_destroy_free(rbuf);
        /* Session token should be non-empty. */
        if (!*token) {
                rd_free(token);
                return NULL;
        }
        return token;
}

/**
 * @brief GET a metadata endpoint with the IMDSv2 session token header.
 *        Returns the response body as a C string on success.
 */
static char *imds_get(rd_kafka_t *rk,
                      const char *endpoint,
                      const char *path,
                      const char *session_token,
                      int *code_out,
                      rd_http_error_t **herrp) {
        char *url;
        char tok_header[256];
        char *headers[1];
        rd_http_error_t *herr;
        rd_buf_t *rbuf = NULL;
        int code       = -1;
        char *body     = NULL;

        *herrp = NULL;
        if (code_out)
                *code_out = -1;

        rd_snprintf(tok_header, sizeof(tok_header),
                    "X-aws-ec2-metadata-token: %s", session_token);
        headers[0] = tok_header;

        url  = rd_aws_build_url(endpoint, path);
        herr = rd_http_get(rk, url, headers, 1, IMDS_HTTP_TIMEOUT_S, 1, 100,
                           &rbuf, NULL, &code);
        rd_free(url);

        if (code_out)
                *code_out = code;

        if (herr) {
                *herrp = herr;
                RD_IF_FREE(rbuf, rd_buf_destroy_free);
                return NULL;
        }
        if (!rbuf) {
                return NULL;
        }

        body = rd_aws_buf_to_cstr(rbuf);
        rd_buf_destroy_free(rbuf);
        return body;
}

/**
 * @brief Parse an AWS credentials JSON response into a credentials object.
 *
 * Shape is identical for IMDSv2 (role creds endpoint) and the ECS container
 * credentials agents (classic ECS, Fargate, EKS Pod Identity):
 *   { "AccessKeyId":"...", "SecretAccessKey":"...", "Token":"...",
 *     "Expiration":"ISO-8601 UTC", ... }
 *
 * @param source_label   prefix used in error messages (e.g. "IMDSv2",
 *                       "ECS container credentials"). Not owned, not copied.
 *
 * @returns a new credentials ref on success, or NULL on parse error (with
 *          \p errstr populated).
 */
static rd_kafka_aws_credentials_t *
aws_creds_parse_json_response(const char *source_label,
                              const char *json_str,
                              char *errstr,
                              size_t errstr_size) {
        cJSON *json = NULL;
        cJSON *akid_j, *secret_j, *token_j, *exp_j;
        rd_kafka_aws_credentials_t *creds = NULL;
        int64_t exp_epoch;
        rd_ts_t exp_us = 0;

        json = cJSON_Parse(json_str);
        if (!json) {
                rd_snprintf(errstr, errstr_size,
                            "%s response is not valid JSON", source_label);
                return NULL;
        }

        akid_j   = cJSON_GetObjectItem(json, "AccessKeyId");
        secret_j = cJSON_GetObjectItem(json, "SecretAccessKey");
        token_j  = cJSON_GetObjectItem(json, "Token");
        exp_j    = cJSON_GetObjectItem(json, "Expiration");

        if (!cJSON_IsString(akid_j) || !akid_j->valuestring ||
            !cJSON_IsString(secret_j) || !secret_j->valuestring ||
            !cJSON_IsString(token_j) || !token_j->valuestring) {
                rd_snprintf(errstr, errstr_size,
                            "%s credentials JSON missing required fields "
                            "(AccessKeyId/SecretAccessKey/Token)",
                            source_label);
                cJSON_Delete(json);
                return NULL;
        }

        if (cJSON_IsString(exp_j) && exp_j->valuestring) {
                if (!rd_aws_parse_iso8601_utc(exp_j->valuestring, &exp_epoch)) {
                        rd_snprintf(errstr, errstr_size,
                                    "%s credentials JSON Expiration is not a "
                                    "valid ISO 8601 timestamp: %s",
                                    source_label, exp_j->valuestring);
                        cJSON_Delete(json);
                        return NULL;
                }
                exp_us = rd_aws_epoch_to_monotonic_us(exp_epoch);
        }

        creds = rd_kafka_aws_credentials_new(akid_j->valuestring,
                                             secret_j->valuestring,
                                             token_j->valuestring, exp_us);
        cJSON_Delete(json);
        return creds;
}

static rd_kafka_aws_creds_result_t
imds_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                      rd_kafka_aws_credentials_t **credsp,
                      char *errstr,
                      size_t errstr_size) {
        imds_opaque_t *o = self->opaque;
        const char *disabled;
        char *session_token = NULL;
        char *role_name     = NULL;
        char *creds_json    = NULL;
        char role_path[512];
        rd_http_error_t *herr = NULL;
        int role_code         = -1;
        int creds_code        = -1;
        size_t role_len;
        rd_kafka_aws_creds_result_t result;

        /* Honour the SDK-standard disable switch before touching the network.
         */
        disabled = rd_getenv("AWS_EC2_METADATA_DISABLED", NULL);
        if (disabled &&
            (!rd_strcasecmp(disabled, "true") || !strcmp(disabled, "1")))
                return RD_KAFKA_AWS_CREDS_SKIP;

        /* 1. PUT session token. */
        session_token = imds_fetch_session_token(self->rk, o->endpoint, &herr);
        if (!session_token) {
                /* Any failure here (connection refused, timeout, non-200)
                 * means IMDS isn't reachable or isn't usable — SKIP so the
                 * chain tries the next provider. */
                if (herr)
                        rd_http_error_destroy(herr);
                return RD_KAFKA_AWS_CREDS_SKIP;
        }

        /* 2. GET role name. */
        role_name = imds_get(self->rk, o->endpoint,
                             "/latest/meta-data/iam/security-credentials/",
                             session_token, &role_code, &herr);
        if (herr) {
                /* 404: no role attached — SKIP. Other HTTP errors on a
                 * reachable IMDS endpoint with a valid session token are
                 * unexpected; treat them as SKIP too so we don't block the
                 * chain. Distinction between SKIP and FATAL here can be
                 * refined in a later milestone if needed. */
                rd_http_error_destroy(herr);
                result = RD_KAFKA_AWS_CREDS_SKIP;
                goto done;
        }
        if (!role_name || !*role_name) {
                result = RD_KAFKA_AWS_CREDS_SKIP;
                goto done;
        }

        /* Trim trailing whitespace (role-name response is text, may end \n). */
        role_len = strlen(role_name);
        while (role_len > 0 && (role_name[role_len - 1] == '\n' ||
                                role_name[role_len - 1] == '\r' ||
                                role_name[role_len - 1] == ' '))
                role_name[--role_len] = '\0';
        if (role_len == 0) {
                result = RD_KAFKA_AWS_CREDS_SKIP;
                goto done;
        }

        /* 3. GET creds for this role. From here on, failure is FATAL —
         * the role exists, so we know IMDS is serving this instance. */
        rd_snprintf(role_path, sizeof(role_path),
                    "/latest/meta-data/iam/security-credentials/%s", role_name);
        creds_json = imds_get(self->rk, o->endpoint, role_path, session_token,
                              &creds_code, &herr);
        if (herr || !creds_json) {
                rd_snprintf(errstr, errstr_size,
                            "IMDSv2: failed to fetch credentials for role "
                            "\"%s\" (http %d): %s",
                            role_name, creds_code,
                            herr ? herr->errstr : "no response body");
                if (herr)
                        rd_http_error_destroy(herr);
                result = RD_KAFKA_AWS_CREDS_FATAL;
                goto done;
        }

        /* 4. Parse + construct credentials. */
        *credsp = aws_creds_parse_json_response("IMDSv2", creds_json, errstr,
                                                errstr_size);
        result  = *credsp ? RD_KAFKA_AWS_CREDS_OK : RD_KAFKA_AWS_CREDS_FATAL;

done:
        if (session_token) {
                /* Scrub before free — treat session token like a secret. */
                memset(session_token, 0, strlen(session_token));
                rd_free(session_token);
        }
        RD_IF_FREE(role_name, rd_free);
        RD_IF_FREE(creds_json, rd_free);
        return result;
}

static void imds_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        imds_opaque_t *o = self->opaque;
        if (o) {
                RD_IF_FREE(o->endpoint, rd_free);
                rd_free(o);
        }
        rd_free(self);
}

rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_imds_new(rd_kafka_t *rk) {
        rd_kafka_aws_creds_provider_t *p;
        imds_opaque_t *o;
        const char *endpoint_env;
        const char *endpoint;
        size_t endpoint_len;

        endpoint_env = rd_getenv("AWS_EC2_METADATA_SERVICE_ENDPOINT", NULL);
        endpoint     = (endpoint_env && *endpoint_env) ? endpoint_env
                                                       : IMDS_DEFAULT_ENDPOINT;

        o           = rd_calloc(1, sizeof(*o));
        o->endpoint = rd_strdup(endpoint);

        /* Strip trailing slash if present so path concatenation is clean. */
        endpoint_len = strlen(o->endpoint);
        if (endpoint_len > 0 && o->endpoint[endpoint_len - 1] == '/')
                o->endpoint[endpoint_len - 1] = '\0';

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "imds";
        p->resolve = imds_provider_resolve;
        p->destroy = imds_provider_destroy;
        p->rk      = rk;
        p->opaque  = o;
        return p;
}


/*
 * ============================================================
 * ECS / Fargate / EKS-Pod-Identity container-credentials provider
 * ============================================================
 *
 * Two operating modes driven by env vars, per the AWS SDK spec:
 *
 *   AWS_CONTAINER_CREDENTIALS_RELATIVE_URI=<path>
 *      -> fetch http://169.254.170.2<path>    (classic ECS/Fargate; no auth)
 *
 *   AWS_CONTAINER_CREDENTIALS_FULL_URI=<url>
 *      -> fetch <url>                          (EKS Pod Identity; allowlisted)
 *      Optional bearer token for FULL_URI mode:
 *        AWS_CONTAINER_AUTHORIZATION_TOKEN       (static string)
 *        AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE  (file path, re-read each
 *                                                 call — Pod Identity rotates)
 *
 * JSON response shape is identical to IMDSv2's role-creds endpoint, so we
 * reuse aws_creds_parse_json_response().
 */

#define ECS_RELATIVE_URI_HOST "http://169.254.170.2"
#define ECS_HTTP_TIMEOUT_S    5


/**
 * @brief Check if \p url has a scheme + host permitted for FULL_URI mode.
 *
 * Allowlist (security boundary):
 *   - localhost / 127.0.0.1 / ::1
 *   - 169.254.170.2   (classic ECS agent)
 *   - 169.254.170.23  (EKS Pod Identity IPv4)
 *   - fd00:ec2::23    (EKS Pod Identity IPv6)
 *
 * Correctly handles the userinfo trick: `http://trusted@evil.com/...` has
 * host `evil.com` — we only check the authority after any `@`, not before.
 * Only `http://` and `https://` schemes accepted.
 */
static rd_bool_t url_host_allowed(const char *url) {
        const char *scheme_end;
        const char *authority_start;
        const char *path_start;
        const char *at;
        const char *host_start;
        const char *host_end;
        char host[256];
        size_t host_len;

        if (!url)
                return rd_false;

        /* Only http / https. */
        if (!strncmp(url, "http://", 7))
                scheme_end = url + 7;
        else if (!strncmp(url, "https://", 8))
                scheme_end = url + 8;
        else
                return rd_false;

        authority_start = scheme_end;
        /* Authority ends at the first /, ?, #, or string end. */
        path_start = authority_start;
        while (*path_start && *path_start != '/' && *path_start != '?' &&
               *path_start != '#')
                path_start++;

        /* If there's a userinfo '@' within the authority, host starts after. */
        at         = memchr(authority_start, '@',
                            (size_t)(path_start - authority_start));
        host_start = at ? at + 1 : authority_start;

        /* IPv6 bracketed form: [addr]:port. Else host ends at : or path. */
        if (*host_start == '[') {
                const char *bracket_end =
                    memchr(host_start, ']', (size_t)(path_start - host_start));
                if (!bracket_end)
                        return rd_false;
                host_start++; /* skip '[' */
                host_end = bracket_end;
        } else {
                host_end = host_start;
                while (host_end < path_start && *host_end != ':')
                        host_end++;
        }

        host_len = (size_t)(host_end - host_start);
        if (host_len == 0 || host_len >= sizeof(host))
                return rd_false;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';

        return !strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") ||
               !strcmp(host, "::1") || !strcmp(host, "169.254.170.2") ||
               !strcmp(host, "169.254.170.23") || !strcmp(host, "fd00:ec2::23");
}


/**
 * @brief Read a bearer/JWT token file and strip trailing whitespace.
 *
 * Used by both the ECS provider (AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE)
 * and the web_identity provider (AWS_WEB_IDENTITY_TOKEN_FILE). Both sources
 * are re-read on every resolve because Kubernetes / Pod Identity rotate the
 * file contents periodically. Kubernetes projected tokens and
 * `echo`-written files end with a trailing newline (confirmed live in
 * Probe C) which must be stripped before use.
 *
 * @returns newly allocated NUL-terminated string (caller frees), or NULL
 *          on error.
 */
static char *aws_read_token_file(const char *path) {
        size_t len;
        char *buf = rd_file_read(path, &len, 1024 * 1024);
        if (!buf)
                return NULL;
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                           buf[len - 1] == ' ' || buf[len - 1] == '\t'))
                buf[--len] = '\0';
        return buf;
}


static rd_kafka_aws_creds_result_t
ecs_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                     rd_kafka_aws_credentials_t **credsp,
                     char *errstr,
                     size_t errstr_size) {
        const char *relative_uri =
            rd_getenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", NULL);
        const char *full_uri =
            rd_getenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", NULL);
        const char *auth_token_env =
            rd_getenv("AWS_CONTAINER_AUTHORIZATION_TOKEN", NULL);
        const char *auth_token_file =
            rd_getenv("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", NULL);

        char *url                 = NULL;
        char *token_file_contents = NULL;
        const char *token         = NULL;
        char *auth_header         = NULL;
        char *headers_array[1]    = {NULL};
        int n_headers             = 0;
        rd_http_error_t *herr     = NULL;
        rd_buf_t *rbuf            = NULL;
        int code                  = -1;
        char *creds_json          = NULL;
        rd_kafka_aws_creds_result_t result;

        /* Treat empty-string env vars as unset. */
        if (relative_uri && !*relative_uri)
                relative_uri = NULL;
        if (full_uri && !*full_uri)
                full_uri = NULL;
        if (auth_token_env && !*auth_token_env)
                auth_token_env = NULL;
        if (auth_token_file && !*auth_token_file)
                auth_token_file = NULL;

        /* Mode detection. FULL_URI wins if both are somehow set (matches
         * SDK precedence). */
        if (full_uri) {
                if (!url_host_allowed(full_uri)) {
                        rd_snprintf(
                            errstr, errstr_size,
                            "ECS: AWS_CONTAINER_CREDENTIALS_FULL_URI host "
                            "is not in the allowed set "
                            "(loopback, 169.254.170.2, 169.254.170.23, "
                            "[fd00:ec2::23]): %s",
                            full_uri);
                        return RD_KAFKA_AWS_CREDS_FATAL;
                }
                url = rd_strdup(full_uri);
        } else if (relative_uri) {
                size_t url_size =
                    strlen(ECS_RELATIVE_URI_HOST) + strlen(relative_uri) + 2;
                url = rd_malloc(url_size);
                if (relative_uri[0] == '/')
                        rd_snprintf(url, url_size, "%s%s",
                                    ECS_RELATIVE_URI_HOST, relative_uri);
                else
                        rd_snprintf(url, url_size, "%s/%s",
                                    ECS_RELATIVE_URI_HOST, relative_uri);
        } else {
                /* Neither env var set — not applicable. */
                return RD_KAFKA_AWS_CREDS_SKIP;
        }

        /* Resolve auth token. File trumps static env; Pod Identity rotates
         * the file so we re-read every resolve. Only applicable in
         * FULL_URI mode — classic ECS uses source-IP authentication at
         * the agent, no Authorization header. */
        if (full_uri) {
                if (auth_token_file) {
                        token_file_contents =
                            aws_read_token_file(auth_token_file);
                        if (!token_file_contents) {
                                rd_snprintf(errstr, errstr_size,
                                            "ECS: failed to read "
                                            "AWS_CONTAINER_AUTHORIZATION_TOKEN_"
                                            "FILE at %s",
                                            auth_token_file);
                                result = RD_KAFKA_AWS_CREDS_FATAL;
                                goto done;
                        }
                        token = token_file_contents;
                } else if (auth_token_env) {
                        token = auth_token_env;
                }
        }

        if (token) {
                size_t hlen = strlen(token) + 32;
                auth_header = rd_malloc(hlen);
                rd_snprintf(auth_header, hlen, "Authorization: %s", token);
                headers_array[0] = auth_header;
                n_headers        = 1;
        }

        /* Fetch the credentials JSON. */
        herr = rd_http_get(self->rk, url, headers_array, n_headers,
                           ECS_HTTP_TIMEOUT_S, 1, 100, &rbuf, NULL, &code);
        if (herr) {
                rd_snprintf(errstr, errstr_size,
                            "ECS container credentials fetch failed "
                            "(HTTP %d): %s",
                            code, herr->errstr);
                rd_http_error_destroy(herr);
                RD_IF_FREE(rbuf, rd_buf_destroy_free);
                result = RD_KAFKA_AWS_CREDS_FATAL;
                goto done;
        }
        if (!rbuf || rd_buf_len(rbuf) == 0) {
                rd_snprintf(errstr, errstr_size,
                            "ECS container credentials: empty response body "
                            "from %s",
                            url);
                RD_IF_FREE(rbuf, rd_buf_destroy_free);
                result = RD_KAFKA_AWS_CREDS_FATAL;
                goto done;
        }

        creds_json = rd_aws_buf_to_cstr(rbuf);
        rd_buf_destroy_free(rbuf);
        rbuf = NULL;

        *credsp = aws_creds_parse_json_response(
            "ECS container credentials", creds_json, errstr, errstr_size);
        result = *credsp ? RD_KAFKA_AWS_CREDS_OK : RD_KAFKA_AWS_CREDS_FATAL;

done:
        RD_IF_FREE(url, rd_free);
        RD_IF_FREE(creds_json, rd_free);
        RD_IF_FREE(auth_header, rd_free);
        if (token_file_contents) {
                /* Scrub bearer token on free — best-effort. */
                memset(token_file_contents, 0, strlen(token_file_contents));
                rd_free(token_file_contents);
        }
        return result;
}

static void ecs_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        /* No opaque state — all config is read from env on each resolve so
         * that rotating tokens / changing endpoints are picked up. */
        rd_free(self);
}

rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_ecs_new(rd_kafka_t *rk) {
        rd_kafka_aws_creds_provider_t *p;

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "ecs";
        p->resolve = ecs_provider_resolve;
        p->destroy = ecs_provider_destroy;
        p->rk      = rk;
        return p;
}


/*
 * ============================================================
 * web_identity provider — EKS IRSA path
 * ============================================================
 *
 *   AWS_WEB_IDENTITY_TOKEN_FILE -> path to JWT signed by the k8s OIDC
 *                                 provider (rotated periodically)
 *   AWS_ROLE_ARN                -> IAM role to assume
 *   AWS_ROLE_SESSION_NAME       -> optional; auto-generated if absent
 *
 * On resolve:
 *   1. Re-read the JWT from disk (handles k8s token rotation).
 *   2. Call sts:AssumeRoleWithWebIdentity (unauthenticated; the JWT is
 *      the identity proof — see
 * rd_kafka_aws_sts_assume_role_with_web_identity).
 *   3. Return the resulting temporary credentials.
 */

/* Prefix for auto-generated session names. AWS allows [\w+=,.@-]{2,64}. */
#define WEB_IDENTITY_SESSION_NAME_PREFIX "librdkafka"

typedef struct web_identity_opaque_s {
        char *region; /* captured at provider-new time; STS call requires it */
} web_identity_opaque_t;

static rd_kafka_aws_creds_result_t
web_identity_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                              rd_kafka_aws_credentials_t **credsp,
                              char *errstr,
                              size_t errstr_size) {
        web_identity_opaque_t *o = self->opaque;
        const char *token_file_path =
            rd_getenv("AWS_WEB_IDENTITY_TOKEN_FILE", NULL);
        const char *role_arn = rd_getenv("AWS_ROLE_ARN", NULL);
        const char *role_session_name_env =
            rd_getenv("AWS_ROLE_SESSION_NAME", NULL);
        char *token_contents = NULL;
        char session_name_buf[128];
        const char *session_name;
        rd_kafka_resp_err_t err;
        rd_kafka_aws_creds_result_t result;

        if (token_file_path && !*token_file_path)
                token_file_path = NULL;
        if (role_arn && !*role_arn)
                role_arn = NULL;
        if (role_session_name_env && !*role_session_name_env)
                role_session_name_env = NULL;

        /* Both required env vars must be present — otherwise this provider
         * is not applicable on this deployment. */
        if (!token_file_path || !role_arn)
                return RD_KAFKA_AWS_CREDS_SKIP;

        token_contents = aws_read_token_file(token_file_path);
        if (!token_contents || !*token_contents) {
                rd_snprintf(errstr, errstr_size,
                            "web_identity: failed to read non-empty content "
                            "from AWS_WEB_IDENTITY_TOKEN_FILE=%s",
                            token_file_path);
                RD_IF_FREE(token_contents, rd_free);
                return RD_KAFKA_AWS_CREDS_FATAL;
        }

        if (role_session_name_env) {
                session_name = role_session_name_env;
        } else {
                rd_snprintf(session_name_buf, sizeof(session_name_buf),
                            "%s-%llu", WEB_IDENTITY_SESSION_NAME_PREFIX,
                            (unsigned long long)time(NULL));
                session_name = session_name_buf;
        }

        err = rd_kafka_aws_sts_assume_role_with_web_identity(
            self->rk, o->region, role_arn, session_name, token_contents, credsp,
            errstr, errstr_size);

        /* Scrub the token before free — it's a bearer credential. */
        memset(token_contents, 0, strlen(token_contents));
        rd_free(token_contents);

        result = (err == RD_KAFKA_RESP_ERR_NO_ERROR) ? RD_KAFKA_AWS_CREDS_OK
                                                     : RD_KAFKA_AWS_CREDS_FATAL;
        return result;
}

static void web_identity_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        web_identity_opaque_t *o = self->opaque;
        if (o) {
                RD_IF_FREE(o->region, rd_free);
                rd_free(o);
        }
        rd_free(self);
}

rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_web_identity_new(rd_kafka_t *rk,
                                             const char *region) {
        rd_kafka_aws_creds_provider_t *p;
        web_identity_opaque_t *o;

        rd_assert(region && *region);

        o         = rd_calloc(1, sizeof(*o));
        o->region = rd_strdup(region);

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "web_identity";
        p->resolve = web_identity_provider_resolve;
        p->destroy = web_identity_provider_destroy;
        p->rk      = rk;
        p->opaque  = o;
        return p;
}

#endif /* WITH_CURL (IMDSv2 + ECS + web_identity providers gated here;         \
        * env+chain above always built) */


/*
 * ============================================================
 * Unit tests (Milestones 1 + 3)
 * ============================================================
 *
 * Gate on WITH_CURL to match the production build — the AWS OAUTHBEARER
 * feature as a whole requires libcurl for the STS call, even though the
 * env provider alone does not.
 */

#if WITH_CURL

#ifdef _WIN32
#define RD_UT_SETENV(k, v) _putenv_s((k), (v))
#define RD_UT_UNSETENV(k)  _putenv_s((k), "")
#else
#include <stdlib.h>
#define RD_UT_SETENV(k, v) setenv((k), (v), 1)
#define RD_UT_UNSETENV(k)  unsetenv((k))
#endif

/**
 * @brief Fake provider for chain tests. Produces a configured result.
 */
typedef struct fake_provider_opaque_s {
        rd_kafka_aws_creds_result_t result;
        const char *akid; /* used if result == OK */
        const char *secret;
        const char *errmsg; /* used if result == FATAL */
        int call_count;     /* incremented on each resolve call */
} fake_provider_opaque_t;

static rd_kafka_aws_creds_result_t
fake_provider_resolve(rd_kafka_aws_creds_provider_t *self,
                      rd_kafka_aws_credentials_t **credsp,
                      char *errstr,
                      size_t errstr_size) {
        fake_provider_opaque_t *o = self->opaque;
        o->call_count++;
        switch (o->result) {
        case RD_KAFKA_AWS_CREDS_OK:
                *credsp =
                    rd_kafka_aws_credentials_new(o->akid, o->secret, NULL, 0);
                return RD_KAFKA_AWS_CREDS_OK;
        case RD_KAFKA_AWS_CREDS_FATAL:
                if (o->errmsg)
                        rd_snprintf(errstr, errstr_size, "%s", o->errmsg);
                return RD_KAFKA_AWS_CREDS_FATAL;
        case RD_KAFKA_AWS_CREDS_SKIP:
        default:
                return RD_KAFKA_AWS_CREDS_SKIP;
        }
}

static void fake_provider_destroy(rd_kafka_aws_creds_provider_t *self) {
        rd_free(self->opaque);
        rd_free(self);
}

static rd_kafka_aws_creds_provider_t *
fake_provider_new(rd_kafka_aws_creds_result_t result,
                  const char *akid,
                  const char *secret,
                  const char *errmsg) {
        rd_kafka_aws_creds_provider_t *p;
        fake_provider_opaque_t *o;
        o         = rd_calloc(1, sizeof(*o));
        o->result = result;
        o->akid   = akid;
        o->secret = secret;
        o->errmsg = errmsg;

        p          = rd_calloc(1, sizeof(*p));
        p->name    = "fake";
        p->resolve = fake_provider_resolve;
        p->destroy = fake_provider_destroy;
        p->opaque  = o;
        return p;
}


/**
 * @brief Credentials struct lifecycle: create, keep, destroy.
 */
static int ut_credentials_lifecycle(void) {
        rd_kafka_aws_credentials_t *c;
        rd_kafka_aws_credentials_t *c2;

        RD_UT_BEGIN();

        c = rd_kafka_aws_credentials_new("AKID", "SECRET", "TOKEN", 0);
        RD_UT_ASSERT(c != NULL, "new returned NULL");
        RD_UT_ASSERT(!strcmp(c->access_key_id, "AKID"), "akid mismatch: %s",
                     c->access_key_id);
        RD_UT_ASSERT(!strcmp(c->secret_access_key, "SECRET"),
                     "secret mismatch");
        RD_UT_ASSERT(!strcmp(c->session_token, "TOKEN"), "token mismatch");
        RD_UT_ASSERT(c->expiration_us == 0,
                     "expiration_us expected 0, got %" PRId64,
                     c->expiration_us);
        RD_UT_ASSERT(rd_refcnt_get(&c->refcnt) == 1,
                     "refcount expected 1, got %d", rd_refcnt_get(&c->refcnt));

        c2 = rd_kafka_aws_credentials_keep(c);
        RD_UT_ASSERT(c2 == c, "keep must return same pointer");
        RD_UT_ASSERT(rd_refcnt_get(&c->refcnt) == 2,
                     "refcount after keep expected 2, got %d",
                     rd_refcnt_get(&c->refcnt));

        rd_kafka_aws_credentials_destroy(c2);
        RD_UT_ASSERT(rd_refcnt_get(&c->refcnt) == 1,
                     "refcount after first destroy expected 1, got %d",
                     rd_refcnt_get(&c->refcnt));

        rd_kafka_aws_credentials_destroy(c);
        /* c is freed now; can't inspect further. ASAN verifies the leak
         * path if the refcount was wrong. */

        /* Long-lived credentials (no session token). */
        c = rd_kafka_aws_credentials_new("AKID", "SECRET", NULL, 0);
        RD_UT_ASSERT(c->session_token == NULL,
                     "session_token expected NULL for long-lived creds");
        rd_kafka_aws_credentials_destroy(c);

        /* Empty-string session token normalized to NULL. */
        c = rd_kafka_aws_credentials_new("AKID", "SECRET", "", 0);
        RD_UT_ASSERT(c->session_token == NULL,
                     "empty session_token must be normalized to NULL");
        rd_kafka_aws_credentials_destroy(c);

        /* NULL-passthrough destroy. */
        rd_kafka_aws_credentials_destroy(NULL);

        RD_UT_PASS();
}

/**
 * @brief Expiration arithmetic.
 */
static int ut_credentials_expired(void) {
        rd_kafka_aws_credentials_t *c;

        RD_UT_BEGIN();

        /* expiration_us == 0 means never expires. */
        c = rd_kafka_aws_credentials_new("A", "S", NULL, 0);
        RD_UT_ASSERT(!rd_kafka_aws_credentials_expired(c, 1000000),
                     "expiration_us=0 must never be expired");
        RD_UT_ASSERT(
            !rd_kafka_aws_credentials_expired(c, 0x7FFFFFFFFFFFFFFFLL),
            "expiration_us=0 must never be expired, even at INT64_MAX");
        rd_kafka_aws_credentials_destroy(c);

        /* Absolute expiry: expired at or after expiration_us. */
        c = rd_kafka_aws_credentials_new("A", "S", NULL, /*expires at:*/ 500);
        RD_UT_ASSERT(!rd_kafka_aws_credentials_expired(c, 499),
                     "should not be expired just before expiration_us");
        RD_UT_ASSERT(rd_kafka_aws_credentials_expired(c, 500),
                     "should be expired exactly at expiration_us");
        RD_UT_ASSERT(rd_kafka_aws_credentials_expired(c, 1000),
                     "should be expired well past expiration_us");
        rd_kafka_aws_credentials_destroy(c);

        RD_UT_PASS();
}

/**
 * @brief Env provider: table-driven coverage.
 *
 * Saves and restores whatever state the caller's environment had, so the
 * test is safe to run multiple times and in any order.
 */
static int ut_env_provider(void) {
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds;
        rd_kafka_aws_creds_result_t r;
        char errstr[512];
        char *saved_akid, *saved_secret, *saved_token;
        const char *tmp;

        RD_UT_BEGIN();

        /* Stash original values so the test doesn't pollute the env. */
        tmp          = rd_getenv("AWS_ACCESS_KEY_ID", NULL);
        saved_akid   = tmp ? rd_strdup(tmp) : NULL;
        tmp          = rd_getenv("AWS_SECRET_ACCESS_KEY", NULL);
        saved_secret = tmp ? rd_strdup(tmp) : NULL;
        tmp          = rd_getenv("AWS_SESSION_TOKEN", NULL);
        saved_token  = tmp ? rd_strdup(tmp) : NULL;

        p = rd_kafka_aws_creds_provider_env_new(NULL);
        RD_UT_ASSERT(p != NULL, "env provider new returned NULL");
        RD_UT_ASSERT(!strcmp(p->name, "env"), "name mismatch: %s", p->name);

        /* --- Case 1: neither var set -> SKIP. --- */
        RD_UT_UNSETENV("AWS_ACCESS_KEY_ID");
        RD_UT_UNSETENV("AWS_SECRET_ACCESS_KEY");
        RD_UT_UNSETENV("AWS_SESSION_TOKEN");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "case 1 expected SKIP, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds == NULL, "case 1 credsp should be NULL on SKIP");

        /* --- Case 2: only AKID set -> FATAL. --- */
        RD_UT_SETENV("AWS_ACCESS_KEY_ID", "AKIA_TEST");
        RD_UT_UNSETENV("AWS_SECRET_ACCESS_KEY");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                     "case 2 expected FATAL, got %d", r);
        RD_UT_ASSERT(creds == NULL, "case 2 credsp should be NULL on FATAL");
        RD_UT_ASSERT(errstr[0] != '\0',
                     "case 2 errstr should be populated on FATAL");

        /* --- Case 3: only SECRET set -> FATAL. --- */
        RD_UT_UNSETENV("AWS_ACCESS_KEY_ID");
        RD_UT_SETENV("AWS_SECRET_ACCESS_KEY", "SECRET_TEST");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                     "case 3 expected FATAL, got %d", r);

        /* --- Case 4: both AKID + SECRET, no session -> OK long-lived. --- */
        RD_UT_SETENV("AWS_ACCESS_KEY_ID", "AKIA_TEST");
        RD_UT_SETENV("AWS_SECRET_ACCESS_KEY", "SECRET_TEST");
        RD_UT_UNSETENV("AWS_SESSION_TOKEN");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "case 4 expected OK, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds != NULL, "case 4 creds should be non-NULL on OK");
        RD_UT_ASSERT(!strcmp(creds->access_key_id, "AKIA_TEST"),
                     "case 4 akid mismatch");
        RD_UT_ASSERT(!strcmp(creds->secret_access_key, "SECRET_TEST"),
                     "case 4 secret mismatch");
        RD_UT_ASSERT(creds->session_token == NULL,
                     "case 4 token should be NULL");
        rd_kafka_aws_credentials_destroy(creds);
        creds = NULL;

        /* --- Case 5: all three set -> OK with session token. --- */
        RD_UT_SETENV("AWS_SESSION_TOKEN", "TOKEN_TEST");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK, "case 5 expected OK, got %d",
                     r);
        RD_UT_ASSERT(creds->session_token != NULL &&
                         !strcmp(creds->session_token, "TOKEN_TEST"),
                     "case 5 token mismatch");
        rd_kafka_aws_credentials_destroy(creds);
        creds = NULL;

        /* --- Case 6: empty-string values treated as unset -> SKIP. --- */
        RD_UT_SETENV("AWS_ACCESS_KEY_ID", "");
        RD_UT_SETENV("AWS_SECRET_ACCESS_KEY", "");
        RD_UT_SETENV("AWS_SESSION_TOKEN", "");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "case 6 expected SKIP (empty vars), got %d", r);

        rd_kafka_aws_creds_provider_destroy(p);

        /* Restore original env. */
        RD_UT_UNSETENV("AWS_ACCESS_KEY_ID");
        RD_UT_UNSETENV("AWS_SECRET_ACCESS_KEY");
        RD_UT_UNSETENV("AWS_SESSION_TOKEN");
        if (saved_akid) {
                RD_UT_SETENV("AWS_ACCESS_KEY_ID", saved_akid);
                rd_free(saved_akid);
        }
        if (saved_secret) {
                RD_UT_SETENV("AWS_SECRET_ACCESS_KEY", saved_secret);
                rd_free(saved_secret);
        }
        if (saved_token) {
                RD_UT_SETENV("AWS_SESSION_TOKEN", saved_token);
                rd_free(saved_token);
        }

        RD_UT_PASS();
}

/**
 * @brief Chain dispatcher semantics.
 */
static int ut_chain_provider(void) {
        rd_kafka_aws_creds_provider_t *chain;
        rd_kafka_aws_credentials_t *creds;
        rd_kafka_aws_creds_result_t r;
        char errstr[512];

        RD_UT_BEGIN();

        /* --- Case 1: [SKIP, OK] -> OK from second provider. --- */
        {
                rd_kafka_aws_creds_provider_t *p[2];
                p[0]  = fake_provider_new(RD_KAFKA_AWS_CREDS_SKIP, NULL, NULL,
                                          NULL);
                p[1]  = fake_provider_new(RD_KAFKA_AWS_CREDS_OK, "AK2", "SK2",
                                          NULL);
                chain = rd_kafka_aws_creds_provider_chain_new(NULL, p, 2);
                r = rd_kafka_aws_creds_provider_resolve(chain, &creds, errstr,
                                                        sizeof(errstr));
                RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                             "case 1 expected OK, got %d", r);
                RD_UT_ASSERT(!strcmp(creds->access_key_id, "AK2"),
                             "case 1 akid mismatch (should come from p[1])");
                rd_kafka_aws_credentials_destroy(creds);
                rd_kafka_aws_creds_provider_destroy(chain);
        }

        /* --- Case 2: [OK, OK] -> first wins; second is never called. --- */
        {
                rd_kafka_aws_creds_provider_t *p[2];
                fake_provider_opaque_t *o1, *o2;
                p[0]  = fake_provider_new(RD_KAFKA_AWS_CREDS_OK, "AK1", "SK1",
                                          NULL);
                p[1]  = fake_provider_new(RD_KAFKA_AWS_CREDS_OK, "AK2", "SK2",
                                          NULL);
                o1    = p[0]->opaque;
                o2    = p[1]->opaque;
                chain = rd_kafka_aws_creds_provider_chain_new(NULL, p, 2);
                r = rd_kafka_aws_creds_provider_resolve(chain, &creds, errstr,
                                                        sizeof(errstr));
                RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK, "case 2 expected OK");
                RD_UT_ASSERT(!strcmp(creds->access_key_id, "AK1"),
                             "case 2 should come from p[0]");
                RD_UT_ASSERT(o1->call_count == 1,
                             "case 2 p[0] expected 1 call, got %d",
                             o1->call_count);
                RD_UT_ASSERT(o2->call_count == 0,
                             "case 2 p[1] expected 0 calls, got %d",
                             o2->call_count);
                rd_kafka_aws_credentials_destroy(creds);
                rd_kafka_aws_creds_provider_destroy(chain);
        }

        /* --- Case 3: [FATAL, OK] -> chain returns FATAL; p[1] not called. */
        {
                rd_kafka_aws_creds_provider_t *p[2];
                fake_provider_opaque_t *o1, *o2;
                p[0]  = fake_provider_new(RD_KAFKA_AWS_CREDS_FATAL, NULL, NULL,
                                          "inner boom");
                p[1]  = fake_provider_new(RD_KAFKA_AWS_CREDS_OK, "AK2", "SK2",
                                          NULL);
                o1    = p[0]->opaque;
                o2    = p[1]->opaque;
                chain = rd_kafka_aws_creds_provider_chain_new(NULL, p, 2);
                r = rd_kafka_aws_creds_provider_resolve(chain, &creds, errstr,
                                                        sizeof(errstr));
                RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                             "case 3 expected FATAL, got %d", r);
                RD_UT_ASSERT(creds == NULL,
                             "case 3 credsp should be NULL on FATAL");
                RD_UT_ASSERT(errstr[0] != '\0',
                             "case 3 errstr should be populated");
                RD_UT_ASSERT(strstr(errstr, "inner boom") != NULL,
                             "case 3 errstr should contain inner message, "
                             "got: %s",
                             errstr);
                RD_UT_ASSERT(strstr(errstr, "fake") != NULL,
                             "case 3 errstr should name provider, got: %s",
                             errstr);
                RD_UT_ASSERT(o1->call_count == 1, "case 3 p[0] call_count");
                RD_UT_ASSERT(o2->call_count == 0,
                             "case 3 p[1] should not be called after FATAL");
                rd_kafka_aws_creds_provider_destroy(chain);
        }

        /* --- Case 4: [SKIP, SKIP] -> chain returns SKIP. --- */
        {
                rd_kafka_aws_creds_provider_t *p[2];
                p[0]  = fake_provider_new(RD_KAFKA_AWS_CREDS_SKIP, NULL, NULL,
                                          NULL);
                p[1]  = fake_provider_new(RD_KAFKA_AWS_CREDS_SKIP, NULL, NULL,
                                          NULL);
                chain = rd_kafka_aws_creds_provider_chain_new(NULL, p, 2);
                r = rd_kafka_aws_creds_provider_resolve(chain, &creds, errstr,
                                                        sizeof(errstr));
                RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                             "case 4 expected SKIP, got %d", r);
                RD_UT_ASSERT(creds == NULL, "case 4 creds NULL");
                rd_kafka_aws_creds_provider_destroy(chain);
        }

        /* --- Case 5: single-provider chain passes through. --- */
        {
                rd_kafka_aws_creds_provider_t *p[1];
                p[0] = fake_provider_new(RD_KAFKA_AWS_CREDS_OK, "A", "S", NULL);
                chain = rd_kafka_aws_creds_provider_chain_new(NULL, p, 1);
                r = rd_kafka_aws_creds_provider_resolve(chain, &creds, errstr,
                                                        sizeof(errstr));
                RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK, "case 5");
                rd_kafka_aws_credentials_destroy(creds);
                rd_kafka_aws_creds_provider_destroy(chain);
        }

        RD_UT_PASS();
}

/*
 * ============================================================
 * ISO 8601 parser — unit test (Milestone 3)
 * ============================================================
 */

static int ut_iso8601(void) {
        int64_t epoch;

        RD_UT_BEGIN();

        /* Classic STS format (Probe B confirmed). */
        RD_UT_ASSERT(rd_aws_parse_iso8601_utc("1970-01-01T00:00:00Z", &epoch),
                     "parse failed");
        RD_UT_ASSERT(epoch == 0, "epoch=%" PRId64, epoch);

        /* GetWebIdentityToken live response format (Probe B). */
        RD_UT_ASSERT(rd_aws_parse_iso8601_utc(
                         "2026-04-21T06:06:47.641000+00:00", &epoch),
                     "parse failed");
        RD_UT_ASSERT(epoch == 1776751607LL,
                     "epoch=%" PRId64 ", expected 1776751607", epoch);

        /* Same wall-moment via negative offset. */
        RD_UT_ASSERT(
            rd_aws_parse_iso8601_utc("2026-04-21T01:06:47-05:00", &epoch),
            "parse failed");
        RD_UT_ASSERT(epoch == 1776751607LL, "negative-offset epoch=%" PRId64,
                     epoch);

        /* Compact offset form +HHMM. */
        RD_UT_ASSERT(
            rd_aws_parse_iso8601_utc("2026-04-21T06:06:47+0000", &epoch),
            "parse failed");
        RD_UT_ASSERT(epoch == 1776751607LL, "compact offset epoch=%" PRId64,
                     epoch);

        /* Malformed -> false. */
        RD_UT_ASSERT(!rd_aws_parse_iso8601_utc("not a timestamp", &epoch),
                     "should fail");
        RD_UT_ASSERT(!rd_aws_parse_iso8601_utc("", &epoch),
                     "empty should fail");
        RD_UT_ASSERT(!rd_aws_parse_iso8601_utc("2026-04-21T06:06:47", &epoch),
                     "no tz should fail");

        RD_UT_PASS();
}


/*
 * ============================================================
 * Minimal in-process HTTP mock server (POSIX-only)
 * ============================================================
 *
 * Binds to 127.0.0.1 on an ephemeral port, serves configured (method, path)
 * routes from a background thread. Single-connection-at-a-time, no
 * keep-alive, no chunked responses. Good enough for IMDS / ECS / STS tests
 * where we control both ends.
 *
 * Not intended for production use, not compiled into the shipping library
 * (only active inside the unit-test block below).
 */

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#define UT_MOCK_MAX_ROUTES       16
#define UT_MOCK_REQUEST_BUF_SIZE 8192

/* MSG_NOSIGNAL is Linux-specific; fall back to 0 elsewhere (macOS uses
 * SO_NOSIGPIPE setsockopt instead; see ut_mock_handle_request). */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Debug tracing gated on RD_UT_MOCK_DEBUG=1 env var. */
static rd_bool_t ut_mock_debug_enabled = rd_false;

#define UT_MOCK_LOG(...)                                                       \
        do {                                                                   \
                if (ut_mock_debug_enabled) {                                   \
                        fprintf(stderr, "[mock] ");                            \
                        fprintf(stderr, __VA_ARGS__);                          \
                        fputc('\n', stderr);                                   \
                        fflush(stderr);                                        \
                }                                                              \
        } while (0)

typedef struct ut_mock_route_s {
        const char *method; /* not owned */
        const char *path;   /* not owned */
        int status;
        const char *content_type; /* not owned */
        char *body;               /* owned, may be NULL */
        size_t body_len;
} ut_mock_route_t;

typedef struct ut_mock_server_s {
        int listen_fd;
        int port;
        thrd_t thread;
        volatile int stop;
        ut_mock_route_t routes[UT_MOCK_MAX_ROUTES];
        int n_routes;
        /* Raw last-request headers — used by tests that assert on
         * inbound Authorization/X-Amz-Security-Token headers. Protected
         * by the mutex; snapshot reader must copy out under the lock. */
        mtx_t lock;
        char last_request[UT_MOCK_REQUEST_BUF_SIZE];
        int last_request_len;
} ut_mock_server_t;

/**
 * @brief Read request bytes until we see the end-of-headers delimiter or
 *        recv times out (SO_RCVTIMEO is set on accepted sockets).
 */
static int ut_mock_read_headers(int fd, char *buf, size_t bufsz) {
        ssize_t total = 0;
        ssize_t n;
        while ((size_t)total < bufsz - 1) {
                n = recv(fd, buf + total, bufsz - 1 - total, 0);
                if (n <= 0) {
                        UT_MOCK_LOG("recv returned %zd errno=%d (%s)", n, errno,
                                    strerror(errno));
                        return -1;
                }
                total += n;
                buf[total] = '\0';
                if (strstr(buf, "\r\n\r\n"))
                        return (int)total;
        }
        UT_MOCK_LOG("recv buffer filled without \\r\\n\\r\\n");
        return -1; /* headers exceed buffer — treat as error */
}

static void ut_mock_handle_request(int client_fd, ut_mock_server_t *s) {
        char reqbuf[UT_MOCK_REQUEST_BUF_SIZE];
        char method[16], path[512];
        char resp[4096];
        int i, resplen, reqlen;
        const ut_mock_route_t *route = NULL;
        ssize_t sent;

        /* 2-second recv timeout so a client that connects but never sends
         * the request line doesn't wedge the mock thread. */
        {
                struct timeval tv;
                tv.tv_sec  = 2;
                tv.tv_usec = 0;
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                           (const char *)&tv, sizeof(tv));
        }
#ifdef SO_NOSIGPIPE
        {
                int on = 1;
                setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on,
                           sizeof(on));
        }
#endif

        reqlen = ut_mock_read_headers(client_fd, reqbuf, sizeof(reqbuf));
        if (reqlen < 0) {
                UT_MOCK_LOG("read_headers failed on fd %d", client_fd);
                return;
        }

        /* Snapshot the request headers for test assertions. */
        mtx_lock(&s->lock);
        if ((size_t)reqlen < sizeof(s->last_request))
                memcpy(s->last_request, reqbuf, (size_t)reqlen + 1);
        else {
                memcpy(s->last_request, reqbuf, sizeof(s->last_request) - 1);
                s->last_request[sizeof(s->last_request) - 1] = '\0';
        }
        s->last_request_len = reqlen;
        mtx_unlock(&s->lock);

        if (sscanf(reqbuf, "%15s %511s ", method, path) != 2) {
                UT_MOCK_LOG("failed to parse request line");
                return;
        }

        UT_MOCK_LOG("request: %s %s", method, path);

        for (i = 0; i < s->n_routes; i++) {
                if (!strcmp(s->routes[i].method, method) &&
                    !strcmp(s->routes[i].path, path)) {
                        route = &s->routes[i];
                        break;
                }
        }

        if (route) {
                UT_MOCK_LOG("matched route #%d status=%d body_len=%" PRIusz, i,
                            route->status, route->body_len);
                resplen = rd_snprintf(resp, sizeof(resp),
                                      "HTTP/1.1 %d OK\r\n"
                                      "Content-Type: %s\r\n"
                                      "Content-Length: %" PRIusz
                                      "\r\n"
                                      "Connection: close\r\n"
                                      "\r\n",
                                      route->status,
                                      route->content_type ? route->content_type
                                                          : "text/plain",
                                      route->body_len);
                sent    = send(client_fd, resp, (size_t)resplen, MSG_NOSIGNAL);
                UT_MOCK_LOG("sent headers: %zd bytes", sent);
                if (route->body && route->body_len > 0) {
                        sent = send(client_fd, route->body, route->body_len,
                                    MSG_NOSIGNAL);
                        UT_MOCK_LOG("sent body: %zd bytes", sent);
                }
        } else {
                UT_MOCK_LOG("no route matched %s %s -> 404", method, path);
                resplen = rd_snprintf(resp, sizeof(resp),
                                      "HTTP/1.1 404 Not Found\r\n"
                                      "Content-Length: 0\r\n"
                                      "Connection: close\r\n"
                                      "\r\n");
                send(client_fd, resp, (size_t)resplen, MSG_NOSIGNAL);
        }
}

static int ut_mock_server_thread(void *arg) {
        ut_mock_server_t *s = arg;
        UT_MOCK_LOG("thread started, listen_fd=%d port=%d", s->listen_fd,
                    s->port);
        while (!s->stop) {
                fd_set rfds;
                struct timeval tv;
                int selret;
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int client_fd;

                /* select() with 500ms timeout so the stop flag is checked
                 * periodically regardless of whether accept() is blocked. */
                FD_ZERO(&rfds);
                FD_SET(s->listen_fd, &rfds);
                tv.tv_sec  = 0;
                tv.tv_usec = 500000;

                selret = select(s->listen_fd + 1, &rfds, NULL, NULL, &tv);
                if (selret < 0) {
                        if (s->stop || errno == EBADF || errno == EINVAL)
                                break;
                        if (errno == EINTR)
                                continue;
                        UT_MOCK_LOG("select errno=%d (%s)", errno,
                                    strerror(errno));
                        break;
                }
                if (selret == 0) /* timeout, re-check stop */
                        continue;

                client_fd =
                    accept(s->listen_fd, (struct sockaddr *)&caddr, &clen);
                if (client_fd < 0) {
                        if (s->stop || errno == EBADF || errno == EINVAL)
                                break;
                        UT_MOCK_LOG("accept errno=%d (%s)", errno,
                                    strerror(errno));
                        continue;
                }
                UT_MOCK_LOG("accepted fd=%d", client_fd);
                ut_mock_handle_request(client_fd, s);
                close(client_fd);
                UT_MOCK_LOG("closed fd=%d", client_fd);
        }
        UT_MOCK_LOG("thread exiting");
        return 0;
}

static ut_mock_server_t *ut_mock_server_new(void) {
        ut_mock_server_t *s;
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int opt        = 1;

        s = rd_calloc(1, sizeof(*s));
        mtx_init(&s->lock, mtx_plain);
        s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->listen_fd < 0)
                goto fail;

        setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt,
                   sizeof(opt));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;

        if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(s->listen_fd, 8) < 0 ||
            getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) < 0)
                goto fail;

        s->port = ntohs(addr.sin_port);

        if (thrd_create(&s->thread, ut_mock_server_thread, s) != thrd_success)
                goto fail;

        return s;

fail:
        if (s->listen_fd >= 0)
                close(s->listen_fd);
        mtx_destroy(&s->lock);
        rd_free(s);
        return NULL;
}

/**
 * @brief Copy the most recent request's full header block into \p out
 *        (NUL-terminated). Returns bytes written (excluding NUL), or 0 if
 *        the mock has not yet received a request.
 */
static size_t
ut_mock_server_last_request(ut_mock_server_t *s, char *out, size_t out_size) {
        size_t copied = 0;
        mtx_lock(&s->lock);
        if (s->last_request_len > 0 && out_size > 0) {
                copied = (size_t)s->last_request_len;
                if (copied >= out_size)
                        copied = out_size - 1;
                memcpy(out, s->last_request, copied);
                out[copied] = '\0';
        } else if (out_size > 0) {
                out[0] = '\0';
        }
        mtx_unlock(&s->lock);
        return copied;
}

static void ut_mock_server_add(ut_mock_server_t *s,
                               const char *method,
                               const char *path,
                               int status,
                               const char *content_type,
                               const char *body) {
        ut_mock_route_t *r;
        rd_assert(s->n_routes < UT_MOCK_MAX_ROUTES);
        r               = &s->routes[s->n_routes++];
        r->method       = method;
        r->path         = path;
        r->status       = status;
        r->content_type = content_type;
        if (body) {
                r->body_len = strlen(body);
                r->body     = rd_strdup(body);
        }
}

static void ut_mock_server_destroy(ut_mock_server_t *s) {
        int i;
        s->stop = 1;
        /* Close listen fd to unblock accept(); thread checks stop flag. */
        close(s->listen_fd);
        s->listen_fd = -1;
        thrd_join(s->thread, NULL);
        for (i = 0; i < s->n_routes; i++)
                RD_IF_FREE(s->routes[i].body, rd_free);
        mtx_destroy(&s->lock);
        rd_free(s);
}


/*
 * ============================================================
 * IMDSv2 provider — unit tests (Milestone 3)
 * ============================================================
 */

/* Helper: build a canned IMDSv2 role-creds JSON body with a future
 * Expiration. Caller frees the result. */
static char *ut_imds_canned_creds_json(const char *akid,
                                       const char *secret,
                                       const char *token,
                                       const char *exp_iso8601) {
        size_t need;
        char *buf;
        need = 256 + strlen(akid) + strlen(secret) + strlen(token) +
               strlen(exp_iso8601);
        buf = rd_malloc(need);
        rd_snprintf(buf, need,
                    "{\n"
                    "  \"Code\": \"Success\",\n"
                    "  \"LastUpdated\": \"2026-04-21T05:00:00Z\",\n"
                    "  \"Type\": \"AWS-HMAC\",\n"
                    "  \"AccessKeyId\": \"%s\",\n"
                    "  \"SecretAccessKey\": \"%s\",\n"
                    "  \"Token\": \"%s\",\n"
                    "  \"Expiration\": \"%s\"\n"
                    "}",
                    akid, secret, token, exp_iso8601);
        return buf;
}

/* For rdhttp calls, we need a non-NULL rd_kafka_t. A zero-inited one is
 * enough for these tests because rd_kafka_terminating() and the SSL CA
 * path access read nullable fields that are NULL on a calloc'd struct. */
static rd_kafka_t *ut_fake_rk(void) {
        return rd_calloc(1, sizeof(rd_kafka_t));
}

static void ut_fake_rk_destroy(rd_kafka_t *rk) {
        rd_free(rk);
}

/**
 * @brief IMDSv2 happy path: mock returns token, role, creds; provider
 *        returns OK with parsed credentials.
 */
static int ut_imds_happy_path(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        char url[128];
        char *creds_json;
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");

        creds_json = ut_imds_canned_creds_json("AKIA_MOCK", "SECRET_MOCK",
                                               "SESSION_TOKEN_MOCK",
                                               "2099-01-01T00:00:00Z");

        ut_mock_server_add(s, "PUT", "/latest/api/token", 200, "text/plain",
                           "FAKE_SESSION_TOKEN_123");
        ut_mock_server_add(s, "GET",
                           "/latest/meta-data/iam/security-credentials/", 200,
                           "text/plain", "my-test-role\n");
        ut_mock_server_add(
            s, "GET", "/latest/meta-data/iam/security-credentials/my-test-role",
            200, "application/json", creds_json);
        rd_free(creds_json);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d", s->port);
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_SETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT", url);

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds != NULL, "creds NULL");
        RD_UT_ASSERT(!strcmp(creds->access_key_id, "AKIA_MOCK"), "akid=%s",
                     creds->access_key_id);
        RD_UT_ASSERT(!strcmp(creds->secret_access_key, "SECRET_MOCK"),
                     "secret mismatch");
        RD_UT_ASSERT(creds->session_token &&
                         !strcmp(creds->session_token, "SESSION_TOKEN_MOCK"),
                     "token mismatch");
        RD_UT_ASSERT(creds->expiration_us > 0,
                     "expiration_us should be set, got %" PRId64,
                     creds->expiration_us);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);

        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");
        RD_UT_PASS();
}

/**
 * @brief AWS_EC2_METADATA_DISABLED=true short-circuits to SKIP without
 *        touching the network.
 */
static int ut_imds_disabled(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();

        RD_UT_SETENV("AWS_EC2_METADATA_DISABLED", "true");
        /* Even if we pointed the endpoint at an unreachable host, the
         * provider must SKIP without hitting it. */
        RD_UT_SETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT", "http://127.0.0.1:1");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "expected SKIP, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds == NULL, "creds must be NULL on SKIP");

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");
        RD_UT_PASS();
}

/**
 * @brief IMDS unreachable (no process listening) → SKIP (not FATAL).
 *        Simulates "not running on EC2."
 */
static int ut_imds_unreachable(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();

        /* Port 1 is reserved; very unlikely to accidentally match. */
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_SETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT", "http://127.0.0.1:1");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "expected SKIP, got %d (errstr=%s)", r, errstr);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");
        RD_UT_PASS();
}

/**
 * @brief IMDS reachable but no role attached (role-list endpoint returns
 *        empty body). Provider should SKIP.
 */
static int ut_imds_no_role(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        char url[128];
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        ut_mock_server_add(s, "PUT", "/latest/api/token", 200, "text/plain",
                           "FAKE_TOKEN");
        ut_mock_server_add(s, "GET",
                           "/latest/meta-data/iam/security-credentials/", 200,
                           "text/plain", "");

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d", s->port);
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_SETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT", url);

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "expected SKIP (no role), got %d (errstr=%s)", r, errstr);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");
        RD_UT_PASS();
}

/**
 * @brief Role attached but creds endpoint returns malformed JSON → FATAL.
 */
static int ut_imds_malformed_json(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        char url[128];
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        ut_mock_server_add(s, "PUT", "/latest/api/token", 200, "text/plain",
                           "FAKE_TOKEN");
        ut_mock_server_add(s, "GET",
                           "/latest/meta-data/iam/security-credentials/", 200,
                           "text/plain", "my-role");
        /* Valid JSON but wrong shape. */
        ut_mock_server_add(
            s, "GET", "/latest/meta-data/iam/security-credentials/my-role", 200,
            "application/json", "{\"this_is\": \"not the right shape\"}");

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d", s->port);
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_SETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT", url);

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                     "expected FATAL (bad JSON shape), got %d", r);
        RD_UT_ASSERT(creds == NULL, "creds NULL on FATAL");
        RD_UT_ASSERT(errstr[0] != '\0', "errstr populated on FATAL");

        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");
        RD_UT_PASS();
}

/*
 * ============================================================
 * ECS / container-credentials provider — unit tests (Milestone 4)
 * ============================================================
 *
 * FULL_URI mode is mock-testable because the target URL is settable via
 * env var. RELATIVE_URI mode would always resolve to
 * http://169.254.170.2/..., which we can't intercept in a unit test, so
 * we cover it only in the real-ECS integration test (ut_ecs_real, gated
 * on RD_UT_ECS=1 — runs in ECS/Fargate/Pod-Identity environments).
 */

/* Canned role-creds JSON (same shape as IMDSv2 role-creds endpoint). */
#define UT_ECS_MOCK_CREDS_JSON                                                 \
        "{\n"                                                                  \
        "  \"AccessKeyId\": \"AKIA_ECS_MOCK\",\n"                              \
        "  \"SecretAccessKey\": \"ECS_SECRET_40_CHARS_LONG_FOR_MOCK_XXX\",\n"  \
        "  \"Token\": \"ECS_SESSION_TOKEN_MOCK_VALUE\",\n"                     \
        "  \"Expiration\": \"2099-01-01T00:00:00Z\",\n"                        \
        "  \"RoleArn\": \"arn:aws:iam::123456789012:role/test\"\n"             \
        "}"

/**
 * @brief url_host_allowed() truth table (M4 security boundary).
 */
static int ut_url_host_allowed(void) {
        RD_UT_BEGIN();

        /* Permitted hosts. */
        RD_UT_ASSERT(url_host_allowed("http://127.0.0.1/creds"),
                     "127.0.0.1 path");
        RD_UT_ASSERT(url_host_allowed("http://127.0.0.1:8080/creds"),
                     "127.0.0.1:port");
        RD_UT_ASSERT(url_host_allowed("https://localhost/creds"),
                     "localhost https");
        RD_UT_ASSERT(url_host_allowed("http://169.254.170.2/v2/creds/role"),
                     "classic ECS agent");
        RD_UT_ASSERT(url_host_allowed("http://169.254.170.23/v1/credentials"),
                     "Pod Identity v4");
        RD_UT_ASSERT(url_host_allowed("http://[fd00:ec2::23]/v1/credentials"),
                     "Pod Identity v6 bracketed");
        RD_UT_ASSERT(url_host_allowed("http://[::1]:9000/"),
                     "IPv6 loopback bracketed with port");

        /* Disallowed hosts. */
        RD_UT_ASSERT(!url_host_allowed("http://192.168.1.1/"),
                     "RFC1918 not allowed");
        RD_UT_ASSERT(!url_host_allowed("http://10.0.0.1/creds"),
                     "RFC1918 not allowed");
        RD_UT_ASSERT(!url_host_allowed("http://example.com/"),
                     "arbitrary domain");
        RD_UT_ASSERT(!url_host_allowed("http://127.0.0.2/"),
                     "strict 127.0.0.1 — no /8 wildcard");
        RD_UT_ASSERT(!url_host_allowed("http://169.254.169.254/"),
                     "IMDS endpoint is not an ECS endpoint");
        RD_UT_ASSERT(!url_host_allowed("http://169.254.170.1/"),
                     "neighbouring link-local IP");

        /* Userinfo trick: real host is the thing after `@`. */
        RD_UT_ASSERT(url_host_allowed("http://evil.com@127.0.0.1/creds"),
                     "userinfo with allowed host");
        RD_UT_ASSERT(!url_host_allowed("http://127.0.0.1@evil.com/creds"),
                     "userinfo with disallowed host");
        RD_UT_ASSERT(!url_host_allowed("http://user:pass@example.com/"),
                     "user:pass userinfo, evil host");

        /* Scheme restriction. */
        RD_UT_ASSERT(!url_host_allowed("file:///etc/passwd"),
                     "non-http scheme");
        RD_UT_ASSERT(!url_host_allowed("ftp://127.0.0.1/"), "ftp not allowed");
        RD_UT_ASSERT(!url_host_allowed("gopher://169.254.170.2/"),
                     "gopher not allowed");

        /* Malformed. */
        RD_UT_ASSERT(!url_host_allowed(""), "empty");
        RD_UT_ASSERT(!url_host_allowed("not-a-url"), "no scheme");
        RD_UT_ASSERT(!url_host_allowed("http://"), "no authority");

        RD_UT_PASS();
}

/** Unset every env var the ECS provider looks at. */
static void ut_ecs_clear_env(void) {
        RD_UT_UNSETENV("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
        RD_UT_UNSETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI");
        RD_UT_UNSETENV("AWS_CONTAINER_AUTHORIZATION_TOKEN");
        RD_UT_UNSETENV("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE");
}

/**
 * @brief No env vars set → SKIP without any network I/O.
 */
static int ut_ecs_unset_skips(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_ecs_clear_env();

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "expected SKIP, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds == NULL, "creds must be NULL on SKIP");

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        RD_UT_PASS();
}

/**
 * @brief FULL_URI to a disallowed host → FATAL, no HTTP attempted.
 */
static int ut_ecs_disallowed_host_fatal(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_ecs_clear_env();
        RD_UT_SETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI",
                     "http://example.com/creds");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                     "expected FATAL on disallowed host, got %d", r);
        RD_UT_ASSERT(strstr(errstr, "not in the allowed set") != NULL,
                     "errstr should explain the allowlist, got: %s", errstr);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        ut_ecs_clear_env();
        RD_UT_PASS();
}

/**
 * @brief FULL_URI happy path with no auth token — classic ECS shape.
 */
static int ut_ecs_full_uri_no_auth(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_ecs_clear_env();

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");
        ut_mock_server_add(s, "GET", "/v1/credentials", 200, "application/json",
                           UT_ECS_MOCK_CREDS_JSON);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/credentials",
                    s->port);
        RD_UT_SETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI", url);

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds != NULL, "creds NULL");
        RD_UT_ASSERT(!strcmp(creds->access_key_id, "AKIA_ECS_MOCK"),
                     "akid mismatch: %s", creds->access_key_id);
        RD_UT_ASSERT(creds->expiration_us > 0, "expiration_us should be set");

        /* Verify no Authorization header was sent. */
        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        RD_UT_ASSERT(strstr(last_req, "Authorization:") == NULL &&
                         strstr(last_req, "authorization:") == NULL,
                     "no-auth mode must not send Authorization header; "
                     "got request:\n%s",
                     last_req);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_ecs_clear_env();
        RD_UT_PASS();
}

/**
 * @brief FULL_URI + AWS_CONTAINER_AUTHORIZATION_TOKEN (static string).
 */
static int ut_ecs_full_uri_static_token(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_ecs_clear_env();

        s = ut_mock_server_new();
        ut_mock_server_add(s, "GET", "/creds", 200, "application/json",
                           UT_ECS_MOCK_CREDS_JSON);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/creds", s->port);
        RD_UT_SETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI", url);
        RD_UT_SETENV("AWS_CONTAINER_AUTHORIZATION_TOKEN",
                     "Bearer STATIC_TOKEN_XYZ");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);

        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        RD_UT_ASSERT(
            strstr(last_req, "Authorization: Bearer STATIC_TOKEN_XYZ") != NULL,
            "Authorization header must contain the static token; got "
            "request:\n%s",
            last_req);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_ecs_clear_env();
        RD_UT_PASS();
}

/**
 * @brief FULL_URI + AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE. Token is read
 *        fresh from the file on each resolve — exercised here with a
 *        trailing newline in the file, which the provider must strip
 *        before putting into the Authorization header.
 */
static int ut_ecs_full_uri_token_file(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;
        FILE *f;
        char tokenfile_path[512] = {0};
        static const char *FILE_TOKEN_VALUE =
            "Bearer EKS_POD_IDENTITY_TOKEN_FROM_FILE";

        RD_UT_BEGIN();
        ut_ecs_clear_env();

        f = rd_file_mkstemp("librdkafka_ut_ecs_token_", "w", tokenfile_path,
                            sizeof(tokenfile_path));
        RD_UT_ASSERT(f != NULL, "failed to create temp token file");
        /* Write token with a trailing newline — simulates k8s projected
         * token rotation (and `echo "..." > file`). */
        fprintf(f, "%s\n", FILE_TOKEN_VALUE);
        fclose(f);

        s = ut_mock_server_new();
        ut_mock_server_add(s, "GET", "/v1/credentials", 200, "application/json",
                           UT_ECS_MOCK_CREDS_JSON);
        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/credentials",
                    s->port);
        RD_UT_SETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI", url);
        RD_UT_SETENV("AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE", tokenfile_path);
        /* _TOKEN_FILE wins over plain _TOKEN — this pair exercises that. */
        RD_UT_SETENV("AWS_CONTAINER_AUTHORIZATION_TOKEN", "SHOULD_NOT_BE_USED");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);

        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        /* The file-sourced token must land verbatim (trailing newline
         * stripped). The plain-env token must NOT appear. */
        {
                char expected_header[256];
                rd_snprintf(expected_header, sizeof(expected_header),
                            "Authorization: %s", FILE_TOKEN_VALUE);
                RD_UT_ASSERT(strstr(last_req, expected_header) != NULL,
                             "Authorization must come from token file with "
                             "trailing whitespace stripped; got request:\n%s",
                             last_req);
        }
        RD_UT_ASSERT(strstr(last_req, "SHOULD_NOT_BE_USED") == NULL,
                     "static _TOKEN should lose to _TOKEN_FILE; request:\n%s",
                     last_req);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_ecs_clear_env();
        unlink(tokenfile_path);
        RD_UT_PASS();
}

/**
 * @brief FULL_URI to a reachable mock that returns malformed JSON → FATAL.
 */
static int ut_ecs_malformed_json(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_ecs_clear_env();

        s = ut_mock_server_new();
        ut_mock_server_add(s, "GET", "/creds", 200, "application/json",
                           "{\"wrong\":\"shape\"}");
        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/creds", s->port);
        RD_UT_SETENV("AWS_CONTAINER_CREDENTIALS_FULL_URI", url);

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_ecs_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL,
                     "expected FATAL on missing fields, got %d", r);
        RD_UT_ASSERT(strstr(errstr, "missing required fields") != NULL,
                     "errstr should mention missing fields; got: %s", errstr);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_ecs_clear_env();
        RD_UT_PASS();
}


/*
 * ============================================================
 * web_identity provider + AssumeRoleWithWebIdentity — tests (Milestone 5)
 * ============================================================
 */

/* Canned AssumeRoleWithWebIdentity success XML (shape confirmed live
 * in Probe C). Note the nested <Credentials> wrapper and the SessionToken
 * field name (not "Token" as in the IMDS/ECS JSON shape). */
#define UT_WEBID_STS_SUCCESS_XML                                               \
        "<AssumeRoleWithWebIdentityResponse "                                  \
        "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"                 \
        "<AssumeRoleWithWebIdentityResult>"                                    \
        "<Credentials>"                                                        \
        "<AccessKeyId>ASIA_WEBID_MOCK</AccessKeyId>"                           \
        "<SecretAccessKey>WEBID_SECRET_40_CHARS_LONG_FOR_MOCK_XXX</"           \
        "SecretAccessKey>"                                                     \
        "<SessionToken>WEBID_SESSION_TOKEN_MOCK_VALUE</SessionToken>"          \
        "<Expiration>2099-01-01T00:00:00Z</Expiration>"                        \
        "</Credentials>"                                                       \
        "<SubjectFromWebIdentityToken>system:serviceaccount:ns:sa"             \
        "</SubjectFromWebIdentityToken>"                                       \
        "<AssumedRoleUser>"                                                    \
        "<Arn>arn:aws:sts::123456789012:assumed-role/mock/session</Arn>"       \
        "<AssumedRoleId>AROA:session</AssumedRoleId>"                          \
        "</AssumedRoleUser>"                                                   \
        "</AssumeRoleWithWebIdentityResult>"                                   \
        "<ResponseMetadata><RequestId>mock</RequestId></ResponseMetadata>"     \
        "</AssumeRoleWithWebIdentityResponse>"

#define UT_WEBID_STS_INVALID_TOKEN_XML                                         \
        "<ErrorResponse "                                                      \
        "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"                 \
        "<Error>"                                                              \
        "<Type>Sender</Type>"                                                  \
        "<Code>InvalidIdentityToken</Code>"                                    \
        "<Message>Mock: the web identity token provided does not match "       \
        "the configured OIDC trust</Message>"                                  \
        "</Error>"                                                             \
        "<RequestId>mock</RequestId>"                                          \
        "</ErrorResponse>"

/**
 * @brief Direct call to rd_kafka_aws_sts_assume_role_with_web_identity():
 *        mock STS returns canned XML, we verify credentials are parsed and
 *        that the request body on the wire contains correctly URL-encoded
 *        RoleArn / WebIdentityToken and no Authorization header.
 */
static int ut_assume_role_with_web_identity_mock_happy(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_resp_err_t err;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");
        ut_mock_server_add(s, "POST", "/", 200, "text/xml",
                           UT_WEBID_STS_SUCCESS_XML);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);
        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);

        rk = ut_fake_rk();

        err = rd_kafka_aws_sts_assume_role_with_web_identity(
            rk, "us-east-1", "arn:aws:iam::123456789012:role/test-role",
            "unit-test-session", "fake.jwt.token", &creds, errstr,
            sizeof(errstr));

        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR_NO_ERROR,
                     "expected NO_ERROR, got %d (errstr=%s)", err, errstr);
        RD_UT_ASSERT(creds != NULL, "creds NULL");
        RD_UT_ASSERT(!strcmp(creds->access_key_id, "ASIA_WEBID_MOCK"),
                     "akid=%s", creds->access_key_id);
        RD_UT_ASSERT(
            creds->session_token &&
                !strcmp(creds->session_token, "WEBID_SESSION_TOKEN_MOCK_VALUE"),
            "session_token=%s",
            creds->session_token ? creds->session_token : "(null)");
        RD_UT_ASSERT(creds->expiration_us > 0,
                     "expiration_us should be set from the 2099 date");

        /* Verify request on the wire. */
        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        RD_UT_ASSERT(strstr(last_req, "POST / HTTP/1.1") != NULL,
                     "expected POST / on the wire; got:\n%s", last_req);
        RD_UT_ASSERT(strstr(last_req, "Authorization") == NULL &&
                         strstr(last_req, "authorization") == NULL,
                     "AssumeRoleWithWebIdentity must be unauthenticated; "
                     "request:\n%s",
                     last_req);
        RD_UT_ASSERT(
            strstr(last_req,
                   "Content-Type: application/x-www-form-urlencoded") != NULL,
            "expected form-urlencoded content-type; request:\n%s", last_req);
        /* Curl URL-encodes role_arn — ':' becomes %3A, '/' becomes %2F.
         * Stored in a local so the RD_UT_ASSERT stringization doesn't
         * reinterpret the literal percent codes as printf conversions. */
        {
                const char *expected_role_arn_enc =
                    "RoleArn=arn%3Aaws%3Aiam%3A%3A123456789012%3Arole%2F"
                    "test-role";
                RD_UT_ASSERT(strstr(last_req, expected_role_arn_enc) != NULL,
                             "expected URL-encoded RoleArn in body; "
                             "request:\n%s",
                             last_req);
        }
        RD_UT_ASSERT(
            strstr(last_req, "WebIdentityToken=fake.jwt.token") != NULL,
            "expected WebIdentityToken in body; request:\n%s", last_req);
        RD_UT_ASSERT(
            strstr(last_req, "RoleSessionName=unit-test-session") != NULL,
            "expected RoleSessionName in body; request:\n%s", last_req);

        rd_kafka_aws_credentials_destroy(creds);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");
        RD_UT_PASS();
}

/**
 * @brief Direct STS call, mock returns an STS-style <ErrorResponse> ->
 *        AUTHENTICATION error with Code + Message surfaced in errstr.
 */
static int ut_assume_role_with_web_identity_mock_error(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char errstr[512] = {0};
        rd_kafka_resp_err_t err;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        ut_mock_server_add(s, "POST", "/", 400, "text/xml",
                           UT_WEBID_STS_INVALID_TOKEN_XML);
        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);
        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);

        rk = ut_fake_rk();

        err = rd_kafka_aws_sts_assume_role_with_web_identity(
            rk, "us-east-1", "arn:aws:iam::123:role/x", "session",
            "fake.jwt.token", &creds, errstr, sizeof(errstr));

        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR__AUTHENTICATION,
                     "expected AUTHENTICATION, got %d (errstr=%s)", err,
                     errstr);
        RD_UT_ASSERT(creds == NULL, "creds must be NULL on error");
        RD_UT_ASSERT(strstr(errstr, "InvalidIdentityToken") != NULL,
                     "errstr should surface STS Code; got: %s", errstr);
        RD_UT_ASSERT(strstr(errstr, "Mock: the web identity token") != NULL,
                     "errstr should surface STS Message; got: %s", errstr);

        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");
        RD_UT_PASS();
}

static void ut_web_identity_clear_env(void) {
        RD_UT_UNSETENV("AWS_WEB_IDENTITY_TOKEN_FILE");
        RD_UT_UNSETENV("AWS_ROLE_ARN");
        RD_UT_UNSETENV("AWS_ROLE_SESSION_NAME");
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");
}

/**
 * @brief web_identity provider: neither required env var set -> SKIP.
 */
static int ut_web_identity_unset_skips(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_web_identity_clear_env();

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_web_identity_new(rk, "us-east-1");
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "expected SKIP, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds == NULL, "creds NULL on SKIP");

        /* Also: only AWS_ROLE_ARN set, no token file -> still SKIP. */
        RD_UT_SETENV("AWS_ROLE_ARN", "arn:aws:iam::123:role/x");
        r = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_SKIP,
                     "only AWS_ROLE_ARN set: expected SKIP, got %d", r);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        ut_web_identity_clear_env();
        RD_UT_PASS();
}

/**
 * @brief web_identity provider: token file env points to a non-existent
 *        path -> FATAL.
 */
static int ut_web_identity_missing_token_file_fatal(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;

        RD_UT_BEGIN();
        ut_web_identity_clear_env();

        RD_UT_SETENV("AWS_WEB_IDENTITY_TOKEN_FILE",
                     "/definitely/does/not/exist/librdkafka-ut-token");
        RD_UT_SETENV("AWS_ROLE_ARN", "arn:aws:iam::123456789012:role/test");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_web_identity_new(rk, "us-east-1");
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_FATAL, "expected FATAL, got %d",
                     r);
        RD_UT_ASSERT(strstr(errstr, "AWS_WEB_IDENTITY_TOKEN_FILE") != NULL,
                     "errstr should reference the env var; got: %s", errstr);

        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);
        ut_web_identity_clear_env();
        RD_UT_PASS();
}

/**
 * @brief Full web_identity happy path: write a JWT to a tmp file, set env
 *        vars, redirect STS to a mock that returns canned creds XML. The
 *        token file has a trailing newline (`echo` / k8s style) that the
 *        provider must strip before sending.
 */
static int ut_web_identity_happy_path(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;
        FILE *f;
        char tokenfile_path[512] = {0};
        static const char *JWT_CONTENT =
            "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.MOCK_K8S_JWT_PAYLOAD."
            "MOCK_K8S_SIGNATURE";

        RD_UT_BEGIN();
        ut_web_identity_clear_env();

        /* Write JWT + trailing newline (k8s-projected-SA-token style). */
        f = rd_file_mkstemp("librdkafka_ut_webid_", "w", tokenfile_path,
                            sizeof(tokenfile_path));
        RD_UT_ASSERT(f != NULL, "failed to create temp token file");
        fprintf(f, "%s\n", JWT_CONTENT);
        fclose(f);

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");
        ut_mock_server_add(s, "POST", "/", 200, "text/xml",
                           UT_WEBID_STS_SUCCESS_XML);
        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);

        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);
        RD_UT_SETENV("AWS_WEB_IDENTITY_TOKEN_FILE", tokenfile_path);
        RD_UT_SETENV("AWS_ROLE_ARN",
                     "arn:aws:iam::123456789012:role/test-role");
        RD_UT_SETENV("AWS_ROLE_SESSION_NAME", "explicit-session-name");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_web_identity_new(rk, "us-east-1");
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);
        RD_UT_ASSERT(creds != NULL, "creds NULL");
        RD_UT_ASSERT(!strcmp(creds->access_key_id, "ASIA_WEBID_MOCK"),
                     "akid=%s", creds->access_key_id);
        RD_UT_ASSERT(
            creds->session_token &&
                !strcmp(creds->session_token, "WEBID_SESSION_TOKEN_MOCK_VALUE"),
            "session_token mismatch");

        /* Verify: token was sent with trailing newline stripped and the
         * explicit session name was used. */
        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        RD_UT_ASSERT(
            strstr(last_req, "WebIdentityToken=eyJhbGciOiJSUzI1NiIs") != NULL,
            "JWT must land in the body; request:\n%s", last_req);
        /* If the trailing \n weren't stripped, it would be URL-encoded as
         * %0A — that MUST NOT appear in the body. Local vars so RD_UT_ASSERT
         * stringization doesn't reinterpret the percent codes. */
        {
                const char *bad_enc_upper = "MOCK_K8S_SIGNATURE%0A";
                const char *bad_enc_lower = "MOCK_K8S_SIGNATURE%0a";
                RD_UT_ASSERT(strstr(last_req, bad_enc_upper) == NULL &&
                                 strstr(last_req, bad_enc_lower) == NULL,
                             "trailing newline must be stripped; "
                             "request:\n%s",
                             last_req);
        }
        RD_UT_ASSERT(
            strstr(last_req, "RoleSessionName=explicit-session-name") != NULL,
            "explicit session name must be used; request:\n%s", last_req);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_web_identity_clear_env();
        unlink(tokenfile_path);
        RD_UT_PASS();
}

/**
 * @brief web_identity provider: env unset for session name -> provider
 *        auto-generates one matching our "librdkafka-<timestamp>" pattern.
 */
static int ut_web_identity_autogenerated_session_name(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char url[128];
        char last_req[UT_MOCK_REQUEST_BUF_SIZE];
        char errstr[512] = {0};
        rd_kafka_aws_creds_result_t r;
        FILE *f;
        char tokenfile_path[512] = {0};

        RD_UT_BEGIN();
        ut_web_identity_clear_env();

        f = rd_file_mkstemp("librdkafka_ut_webid_autosess_", "w",
                            tokenfile_path, sizeof(tokenfile_path));
        RD_UT_ASSERT(f != NULL, "failed to create temp token file");
        fprintf(f, "some.jwt.token");
        fclose(f);

        s = ut_mock_server_new();
        ut_mock_server_add(s, "POST", "/", 200, "text/xml",
                           UT_WEBID_STS_SUCCESS_XML);
        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);

        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);
        RD_UT_SETENV("AWS_WEB_IDENTITY_TOKEN_FILE", tokenfile_path);
        RD_UT_SETENV("AWS_ROLE_ARN",
                     "arn:aws:iam::123456789012:role/test-role");
        /* AWS_ROLE_SESSION_NAME deliberately NOT set. */

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_web_identity_new(rk, "us-east-1");
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK, got %d (errstr=%s)", r, errstr);

        ut_mock_server_last_request(s, last_req, sizeof(last_req));
        RD_UT_ASSERT(strstr(last_req, "RoleSessionName=librdkafka-") != NULL,
                     "auto-generated session name must start with "
                     "\"librdkafka-\"; request:\n%s",
                     last_req);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_mock_server_destroy(s);
        ut_fake_rk_destroy(rk);
        ut_web_identity_clear_env();
        unlink(tokenfile_path);
        RD_UT_PASS();
}


/*
 * ============================================================
 * GetWebIdentityToken STS — unit tests (Milestone 7)
 * ============================================================
 */

/* Canned GetWebIdentityToken success response body (shape confirmed live
 * in Probe B against real STS, Nov-2025 API). */
#define UT_STS_MOCK_JWT                                                        \
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.MOCK_JWT_PAYLOAD.MOCK_SIG"
#define UT_STS_MOCK_SUCCESS_XML                                                \
        "<GetWebIdentityTokenResponse "                                        \
        "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"                 \
        "<GetWebIdentityTokenResult>"                                          \
        "<WebIdentityToken>" UT_STS_MOCK_JWT                                   \
        "</WebIdentityToken>"                                                  \
        "<Expiration>2099-01-01T00:00:00Z</Expiration>"                        \
        "</GetWebIdentityTokenResult>"                                         \
        "<ResponseMetadata>"                                                   \
        "<RequestId>mock-request-id</RequestId>"                               \
        "</ResponseMetadata>"                                                  \
        "</GetWebIdentityTokenResponse>"

#define UT_STS_MOCK_ACCESS_DENIED_XML                                          \
        "<ErrorResponse "                                                      \
        "xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"                 \
        "<Error>"                                                              \
        "<Type>Sender</Type>"                                                  \
        "<Code>AccessDenied</Code>"                                            \
        "<Message>Mock access denied: role lacks "                             \
        "sts:GetWebIdentityToken</Message>"                                    \
        "</Error>"                                                             \
        "<RequestId>mock-request-id</RequestId>"                               \
        "</ErrorResponse>"

/**
 * @brief Mock STS happy path: request lands on our mock server, canned
 *        success XML comes back, parser extracts JWT and Expiration.
 */
static int ut_sts_mock_happy_path(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_credentials_t *creds;
        rd_kafka_aws_sts_jwt_t *jwt = NULL;
        char url[128];
        char errstr[512] = {0};
        rd_kafka_resp_err_t err;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");
        ut_mock_server_add(s, "POST", "/", 200, "text/xml",
                           UT_STS_MOCK_SUCCESS_XML);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);
        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);

        rk    = ut_fake_rk();
        creds = rd_kafka_aws_credentials_new(
            "AKIA_MOCK_AKID", "MOCK_SECRET_ACCESS_KEY_40_CHARS_LONG___",
            "MOCK_SESSION_TOKEN_SIMULATED_TEMP_CREDS", 0);

        err = rd_kafka_aws_sts_get_web_identity_token(
            rk, "us-east-1", "https://librdkafka-mock.example.com", "RS256",
            300, creds, &jwt, errstr, sizeof(errstr));

        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR_NO_ERROR,
                     "expected NO_ERROR, got %d (errstr=%s)", err, errstr);
        RD_UT_ASSERT(jwt != NULL, "jwt NULL");
        RD_UT_ASSERT(jwt->token && !strcmp(jwt->token, UT_STS_MOCK_JWT),
                     "token mismatch: got %s",
                     jwt->token ? jwt->token : "(null)");
        RD_UT_ASSERT(jwt->expiration_us > 0,
                     "expiration_us should be set for 2099 expiry");

        rd_kafka_aws_sts_jwt_destroy(jwt);
        rd_kafka_aws_credentials_destroy(creds);
        ut_fake_rk_destroy(rk);
        ut_mock_server_destroy(s);
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");

        RD_UT_PASS();
}

/**
 * @brief Mock STS error: server returns HTTP 400 with an STS-style
 *        <ErrorResponse>; caller gets AUTHENTICATION error and the
 *        Code+Message surfaced in errstr.
 */
static int ut_sts_mock_error_access_denied(void) {
        ut_mock_server_t *s;
        rd_kafka_t *rk;
        rd_kafka_aws_credentials_t *creds;
        rd_kafka_aws_sts_jwt_t *jwt = NULL;
        char url[128];
        char errstr[512] = {0};
        rd_kafka_resp_err_t err;

        RD_UT_BEGIN();

        s = ut_mock_server_new();
        RD_UT_ASSERT(s != NULL, "mock server new failed");
        ut_mock_server_add(s, "POST", "/", 400, "text/xml",
                           UT_STS_MOCK_ACCESS_DENIED_XML);

        rd_snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s->port);
        RD_UT_SETENV("AWS_STS_ENDPOINT_URL", url);

        rk    = ut_fake_rk();
        creds = rd_kafka_aws_credentials_new("AKIA_MOCK", "SECRET_MOCK",
                                             "TOKEN_MOCK", 0);

        err = rd_kafka_aws_sts_get_web_identity_token(
            rk, "us-east-1", "https://librdkafka-mock.example.com", "RS256",
            300, creds, &jwt, errstr, sizeof(errstr));

        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR__AUTHENTICATION,
                     "expected AUTHENTICATION, got %d (errstr=%s)", err,
                     errstr);
        RD_UT_ASSERT(jwt == NULL, "jwt must be NULL on error");
        RD_UT_ASSERT(strstr(errstr, "AccessDenied") != NULL,
                     "errstr should surface STS Code. Got: %s", errstr);
        RD_UT_ASSERT(strstr(errstr, "Mock access denied") != NULL,
                     "errstr should surface STS Message. Got: %s", errstr);

        rd_kafka_aws_credentials_destroy(creds);
        ut_fake_rk_destroy(rk);
        ut_mock_server_destroy(s);
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");

        RD_UT_PASS();
}

/**
 * @brief Invalid-argument guard: reject zero/out-of-range DurationSeconds
 *        without touching the network.
 */
static int ut_sts_invalid_args(void) {
        rd_kafka_t *rk;
        rd_kafka_aws_credentials_t *creds;
        rd_kafka_aws_sts_jwt_t *jwt = NULL;
        char errstr[512]            = {0};
        rd_kafka_resp_err_t err;

        RD_UT_BEGIN();
        rk    = ut_fake_rk();
        creds = rd_kafka_aws_credentials_new("AK", "SK", NULL, 0);

        /* DurationSeconds out of [60, 3600]. */
        err = rd_kafka_aws_sts_get_web_identity_token(rk, "us-east-1", "aud",
                                                      "RS256", 10, creds, &jwt,
                                                      errstr, sizeof(errstr));
        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR__INVALID_ARG,
                     "duration=10 should be INVALID_ARG, got %d", err);

        /* NULL region. */
        err = rd_kafka_aws_sts_get_web_identity_token(
            rk, NULL, "aud", "RS256", 300, creds, &jwt, errstr, sizeof(errstr));
        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR__INVALID_ARG,
                     "NULL region should be INVALID_ARG, got %d", err);

        rd_kafka_aws_credentials_destroy(creds);
        ut_fake_rk_destroy(rk);
        RD_UT_PASS();
}

/**
 * @brief End-to-end integration test against REAL STS on a real EC2 instance.
 *        Gated on RD_UT_STS=1. Uses the IMDSv2 provider to fetch real
 *        credentials, then calls sts:GetWebIdentityToken and verifies a
 *        valid JWT comes back.
 *
 * Required preconditions on the instance:
 *   - Attached IAM role with sts:GetWebIdentityToken permission (Probe B
 *     already validated this on ktrue-iam-sts-test-role).
 *   - Account has outbound federation enabled.
 *   - RD_UT_AWS_REGION env var set (e.g. eu-north-1). No default — we
 *     refuse to guess.
 */
static int ut_sts_real(void) {
        const char *flag     = rd_getenv("RD_UT_STS", NULL);
        const char *region   = rd_getenv("RD_UT_AWS_REGION", NULL);
        const char *audience = rd_getenv("RD_UT_AWS_AUDIENCE", NULL);
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        rd_kafka_aws_sts_jwt_t *jwt       = NULL;
        rd_kafka_aws_creds_result_t cr;
        rd_kafka_resp_err_t err;
        char errstr[1024] = {0};
        rd_ts_t now_us;
        long long seconds_until_exp;

        if (!flag || strcmp(flag, "1") != 0)
                RD_UT_SKIP(
                    "RD_UT_STS=1 not set; skipping real GetWebIdentityToken "
                    "integration test");
        if (!region || !*region)
                RD_UT_SKIP(
                    "RD_UT_AWS_REGION not set; skipping real STS test "
                    "(librdkafka requires explicit region)");

        /* The IAM policy attached to the caller's role may restrict the
         * allowed audience via the sts:IdentityTokenAudience condition.
         * Default to the audience the AWS CLI smoke-test uses; override
         * via RD_UT_AWS_AUDIENCE if your policy expects a different one. */
        if (!audience || !*audience)
                audience = "https://api.example.com";

        RD_UT_BEGIN();
        RD_UT_SAY("real STS test: region=%s audience=%s", region, audience);

        /* Ensure we hit real STS, not any mock endpoint leaked from a
         * previous test. */
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");

        rk = ut_fake_rk();

        /* 1. Fetch real AWS creds from IMDSv2. */
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        cr = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        if (cr == RD_KAFKA_AWS_CREDS_SKIP) {
                /* Not running on EC2 (or IMDS blocked) — this test needs
                 * IMDS. Skip cleanly rather than fail; ut_sts_real_env
                 * covers the env-sourced-creds path for Lambda etc. */
                rd_kafka_aws_creds_provider_destroy(p);
                ut_fake_rk_destroy(rk);
                RD_UT_SKIP(
                    "IMDSv2 not reachable (errstr=%s); ut_sts_real needs a "
                    "live EC2 IMDSv2 endpoint. For Lambda / env-creds "
                    "testing see ut_sts_real_env.",
                    errstr);
        }
        RD_UT_ASSERT(cr == RD_KAFKA_AWS_CREDS_OK,
                     "IMDSv2 did not return creds (expected OK, got %d, "
                     "errstr=%s)",
                     cr, errstr);
        rd_kafka_aws_creds_provider_destroy(p);

        /* 2. Call real STS. */
        err = rd_kafka_aws_sts_get_web_identity_token(rk, region, audience,
                                                      "RS256", 300, creds, &jwt,
                                                      errstr, sizeof(errstr));
        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR_NO_ERROR,
                     "expected NO_ERROR from real STS, got %d (errstr=%s)", err,
                     errstr);
        RD_UT_ASSERT(jwt != NULL, "jwt NULL");
        RD_UT_ASSERT(jwt->token && strlen(jwt->token) > 100,
                     "JWT unexpectedly short (got %zu bytes)",
                     jwt->token ? strlen(jwt->token) : (size_t)0);
        /* JWTs are three base64url sections separated by dots. */
        {
                const char *first_dot = strchr(jwt->token, '.');
                const char *second_dot =
                    first_dot ? strchr(first_dot + 1, '.') : NULL;
                RD_UT_ASSERT(first_dot && second_dot &&
                                 strchr(second_dot + 1, '.') == NULL,
                             "JWT doesn't have the expected three-segment "
                             "header.payload.signature shape");
        }

        now_us = rd_clock();
        RD_UT_ASSERT(jwt->expiration_us > now_us,
                     "JWT expiration is in the past");
        seconds_until_exp =
            (long long)((jwt->expiration_us - now_us) / 1000000);

        RD_UT_SAY(
            "STS GetWebIdentityToken success: JWT=%zu bytes, "
            "header prefix=\"%.10s\", expires in %lld seconds",
            strlen(jwt->token), jwt->token, seconds_until_exp);

        rd_kafka_aws_sts_jwt_destroy(jwt);
        rd_kafka_aws_credentials_destroy(creds);
        ut_fake_rk_destroy(rk);

        RD_UT_PASS();
}

/**
 * @brief End-to-end integration test: env provider -> real STS.
 *        Simulates the Lambda execution environment, where AWS temp
 *        credentials are pre-populated in environment variables:
 *          AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN.
 *
 * Gated on RD_UT_STS=1 + RD_UT_AWS_REGION + AWS_ACCESS_KEY_ID being set.
 * Skips cleanly when env creds are absent, so unrelated local runs and
 * EC2 runs without this opt-in don't fail.
 *
 * To simulate Lambda on an EC2 instance (quick validation before an
 * actual Lambda deploy):
 *
 *     TOKEN=$(curl -sX PUT "http://169.254.169.254/latest/api/token" \\
 *              -H "X-aws-ec2-metadata-token-ttl-seconds: 300")
 *     ROLE=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" \\
 *            http://169.254.169.254/latest/meta-data/iam/security-credentials/)
 *     CREDS=$(curl -s -H "X-aws-ec2-metadata-token: $TOKEN" \\
 *             "http://169.254.169.254/latest/meta-data/iam/security-credentials/$ROLE")
 *     export AWS_ACCESS_KEY_ID=$(echo "$CREDS" \\
 *              | grep -oE '"AccessKeyId"[^,]*'     | cut -d\\" -f4)
 *     export AWS_SECRET_ACCESS_KEY=$(echo "$CREDS" \\
 *              | grep -oE '"SecretAccessKey"[^,]*' | cut -d\\" -f4)
 *     export AWS_SESSION_TOKEN=$(echo "$CREDS" \\
 *              | grep -oE '"Token"[^,]*'           | cut -d\\" -f4)
 *
 *     RD_UT_STS=1 RD_UT_AWS_REGION=eu-north-1 ... ./test-runner -l
 */
static int ut_sts_real_env(void) {
        const char *flag     = rd_getenv("RD_UT_STS", NULL);
        const char *region   = rd_getenv("RD_UT_AWS_REGION", NULL);
        const char *audience = rd_getenv("RD_UT_AWS_AUDIENCE", NULL);
        const char *akid     = rd_getenv("AWS_ACCESS_KEY_ID", NULL);
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        rd_kafka_aws_sts_jwt_t *jwt       = NULL;
        rd_kafka_aws_creds_result_t cr;
        rd_kafka_resp_err_t err;
        char errstr[1024] = {0};
        rd_ts_t now_us;
        long long seconds_until_exp;

        if (!flag || strcmp(flag, "1") != 0)
                RD_UT_SKIP(
                    "RD_UT_STS=1 not set; skipping env->STS "
                    "integration test");
        if (!region || !*region)
                RD_UT_SKIP(
                    "RD_UT_AWS_REGION not set; skipping env->STS test "
                    "(librdkafka requires explicit region)");
        if (!akid || !*akid)
                RD_UT_SKIP(
                    "AWS_ACCESS_KEY_ID not in env; env provider would SKIP. "
                    "This test simulates Lambda, where the runtime "
                    "pre-populates those vars. On EC2 you can populate them "
                    "from IMDS manually — see the docstring above.");

        if (!audience || !*audience)
                audience = "https://api.example.com";

        RD_UT_BEGIN();
        RD_UT_SAY("env->STS test (simulating Lambda): region=%s audience=%s",
                  region, audience);

        /* Ensure we hit real STS, not any mock endpoint leaked from a
         * previous test. */
        RD_UT_UNSETENV("AWS_STS_ENDPOINT_URL");

        rk = ut_fake_rk();

        /* 1. Fetch creds via env provider (the Lambda path). */
        p  = rd_kafka_aws_creds_provider_env_new(rk);
        cr = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));
        RD_UT_ASSERT(cr == RD_KAFKA_AWS_CREDS_OK,
                     "env provider failed to resolve (expected OK, got "
                     "%d, errstr=%s)",
                     cr, errstr);
        rd_kafka_aws_creds_provider_destroy(p);

        /* 2. Call real STS with env-sourced creds. */
        err = rd_kafka_aws_sts_get_web_identity_token(rk, region, audience,
                                                      "RS256", 300, creds, &jwt,
                                                      errstr, sizeof(errstr));
        RD_UT_ASSERT(err == RD_KAFKA_RESP_ERR_NO_ERROR,
                     "expected NO_ERROR from real STS via env creds, got "
                     "%d (errstr=%s)",
                     err, errstr);
        RD_UT_ASSERT(jwt != NULL, "jwt NULL");
        RD_UT_ASSERT(jwt->token && strlen(jwt->token) > 100,
                     "JWT unexpectedly short (got %zu bytes)",
                     jwt->token ? strlen(jwt->token) : (size_t)0);

        {
                /* Shape sanity: three dot-separated sections. */
                const char *first_dot = strchr(jwt->token, '.');
                const char *second_dot =
                    first_dot ? strchr(first_dot + 1, '.') : NULL;
                RD_UT_ASSERT(first_dot && second_dot &&
                                 strchr(second_dot + 1, '.') == NULL,
                             "JWT doesn't have the expected three-segment "
                             "header.payload.signature shape");
        }

        now_us = rd_clock();
        RD_UT_ASSERT(jwt->expiration_us > now_us,
                     "JWT expiration is in the past");
        seconds_until_exp =
            (long long)((jwt->expiration_us - now_us) / 1000000);

        RD_UT_SAY(
            "env->STS real success: JWT=%zu bytes, header prefix=\"%.10s\", "
            "expires in %lld seconds",
            strlen(jwt->token), jwt->token, seconds_until_exp);

        rd_kafka_aws_sts_jwt_destroy(jwt);
        rd_kafka_aws_credentials_destroy(creds);
        ut_fake_rk_destroy(rk);

        RD_UT_PASS();
}

#endif /* !_WIN32 (mock server uses POSIX sockets) */


/**
 * @brief Integration test: resolve credentials from REAL IMDSv2 at
 *        169.254.169.254. Gated on RD_UT_IMDS=1 because it requires
 *        the link-local endpoint to be reachable (i.e., running on EC2).
 *
 * If the env var is set but IMDS is unreachable or the instance has no
 * attached role, the test fails — that's intentional, since setting the
 * opt-in flag means "I expect IMDS here."
 */
static int ut_imds_real(void) {
        const char *flag = rd_getenv("RD_UT_IMDS", NULL);
        rd_kafka_t *rk;
        rd_kafka_aws_creds_provider_t *p;
        rd_kafka_aws_credentials_t *creds = NULL;
        char errstr[512]                  = {0};
        rd_kafka_aws_creds_result_t r;
        rd_ts_t now_us;
        long long seconds_until_exp;

        if (!flag || strcmp(flag, "1") != 0)
                RD_UT_SKIP(
                    "RD_UT_IMDS=1 not set; skipping real IMDSv2 integration "
                    "test (requires EC2 instance with attached IAM role)");

        RD_UT_BEGIN();

        /* Clear any overrides that previous mock tests may have left behind
         * so we talk to the real 169.254.169.254 endpoint. */
        RD_UT_UNSETENV("AWS_EC2_METADATA_DISABLED");
        RD_UT_UNSETENV("AWS_EC2_METADATA_SERVICE_ENDPOINT");

        rk = ut_fake_rk();
        p  = rd_kafka_aws_creds_provider_imds_new(rk);
        r  = rd_kafka_aws_creds_provider_resolve(p, &creds, errstr,
                                                 sizeof(errstr));

        RD_UT_ASSERT(r == RD_KAFKA_AWS_CREDS_OK,
                     "expected OK from real IMDSv2, got %d (errstr=%s). "
                     "Ensure this EC2 instance has an attached IAM role.",
                     r, errstr);
        RD_UT_ASSERT(creds != NULL, "creds NULL");
        RD_UT_ASSERT(creds->access_key_id && strlen(creds->access_key_id) >= 16,
                     "AKID looks bogus: %s",
                     creds->access_key_id ? creds->access_key_id : "(null)");
        RD_UT_ASSERT(creds->secret_access_key &&
                         strlen(creds->secret_access_key) >= 30,
                     "secret key unexpectedly short");
        RD_UT_ASSERT(
            creds->session_token && strlen(creds->session_token) >= 100,
            "session token unexpectedly short (got %zu bytes)",
            creds->session_token ? strlen(creds->session_token) : (size_t)0);
        RD_UT_ASSERT(creds->expiration_us > 0, "expiration_us not set");

        now_us = rd_clock();
        RD_UT_ASSERT(creds->expiration_us > now_us,
                     "expiration_us is already in the past");
        seconds_until_exp =
            (long long)((creds->expiration_us - now_us) / 1000000);

        RD_UT_SAY(
            "IMDSv2 resolved real credentials: "
            "AKID prefix=%.4s***, session_token=%zu bytes, "
            "expires in %lld seconds",
            creds->access_key_id, strlen(creds->session_token),
            seconds_until_exp);

        rd_kafka_aws_credentials_destroy(creds);
        rd_kafka_aws_creds_provider_destroy(p);
        ut_fake_rk_destroy(rk);

        RD_UT_PASS();
}


int unittest_aws_credentials(void) {
        int fails = 0;

#ifndef _WIN32
        /* Enable verbose mock-server tracing if requested. */
        {
                const char *dbg = rd_getenv("RD_UT_MOCK_DEBUG", NULL);
                if (dbg && (!strcmp(dbg, "1") || !rd_strcasecmp(dbg, "true")))
                        ut_mock_debug_enabled = rd_true;
        }
        /* Ignore SIGPIPE process-wide so a broken mock-client write (e.g.
         * client closed before server's send completes) doesn't kill the
         * test runner. Redundant with MSG_NOSIGNAL / SO_NOSIGPIPE but
         * belt-and-suspenders. */
        signal(SIGPIPE, SIG_IGN);
#endif

        fails += ut_credentials_lifecycle();
        fails += ut_credentials_expired();
        fails += ut_env_provider();
        fails += ut_chain_provider();
        fails += ut_iso8601();
#ifndef _WIN32
        fails += ut_imds_disabled();
        fails += ut_imds_unreachable();
        fails += ut_imds_no_role();
        fails += ut_imds_malformed_json();
        fails += ut_imds_happy_path();
        fails += ut_imds_real(); /* skips unless RD_UT_IMDS=1 */
        fails += ut_url_host_allowed();
        fails += ut_ecs_unset_skips();
        fails += ut_ecs_disallowed_host_fatal();
        fails += ut_ecs_full_uri_no_auth();
        fails += ut_ecs_full_uri_static_token();
        fails += ut_ecs_full_uri_token_file();
        fails += ut_ecs_malformed_json();
        fails += ut_assume_role_with_web_identity_mock_happy();
        fails += ut_assume_role_with_web_identity_mock_error();
        fails += ut_web_identity_unset_skips();
        fails += ut_web_identity_missing_token_file_fatal();
        fails += ut_web_identity_happy_path();
        fails += ut_web_identity_autogenerated_session_name();
        fails += ut_sts_invalid_args();
        fails += ut_sts_mock_happy_path();
        fails += ut_sts_mock_error_access_denied();
        fails +=
            ut_sts_real(); /* skips unless RD_UT_STS=1 + RD_UT_AWS_REGION */
        fails += ut_sts_real_env(); /* skips unless RD_UT_STS=1 +
                                     * RD_UT_AWS_REGION +
                                     * AWS_ACCESS_KEY_ID in env */
#endif

        return fails;
}

#endif /* WITH_CURL */
