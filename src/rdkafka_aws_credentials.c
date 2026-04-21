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
static rd_bool_t rd_aws_parse_iso8601_utc(const char *iso,
                                          int64_t *epoch_seconds) {
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
static rd_ts_t rd_aws_epoch_to_monotonic_us(int64_t exp_epoch) {
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
 * @brief Parse the IMDSv2 credentials JSON blob into a credentials object.
 *
 * @returns a new credentials ref on success, or NULL on parse error (with
 *          \p errstr populated).
 */
static rd_kafka_aws_credentials_t *
imds_parse_creds_json(const char *json_str, char *errstr, size_t errstr_size) {
        cJSON *json = NULL;
        cJSON *akid_j, *secret_j, *token_j, *exp_j;
        rd_kafka_aws_credentials_t *creds = NULL;
        int64_t exp_epoch;
        rd_ts_t exp_us = 0;

        json = cJSON_Parse(json_str);
        if (!json) {
                rd_snprintf(errstr, errstr_size,
                            "IMDSv2 credentials response is not valid JSON");
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
                            "IMDSv2 credentials JSON missing required fields "
                            "(AccessKeyId/SecretAccessKey/Token)");
                cJSON_Delete(json);
                return NULL;
        }

        if (cJSON_IsString(exp_j) && exp_j->valuestring) {
                if (!rd_aws_parse_iso8601_utc(exp_j->valuestring, &exp_epoch)) {
                        rd_snprintf(errstr, errstr_size,
                                    "IMDSv2 credentials JSON Expiration "
                                    "is not a valid ISO 8601 timestamp: %s",
                                    exp_j->valuestring);
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
        *credsp = imds_parse_creds_json(creds_json, errstr, errstr_size);
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

#endif /* WITH_CURL (IMDSv2 provider gated here; env+chain above always built) \
        */


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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define UT_MOCK_MAX_ROUTES       16
#define UT_MOCK_REQUEST_BUF_SIZE 8192

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
} ut_mock_server_t;

/**
 * @brief Read request bytes until we see the end-of-headers delimiter.
 */
static int ut_mock_read_headers(int fd, char *buf, size_t bufsz) {
        ssize_t total = 0;
        ssize_t n;
        while ((size_t)total < bufsz - 1) {
                n = recv(fd, buf + total, bufsz - 1 - total, 0);
                if (n <= 0)
                        return -1;
                total += n;
                buf[total] = '\0';
                if (strstr(buf, "\r\n\r\n"))
                        return (int)total;
        }
        return -1; /* headers exceed buffer — treat as error */
}

static void ut_mock_handle_request(int client_fd, ut_mock_server_t *s) {
        char reqbuf[UT_MOCK_REQUEST_BUF_SIZE];
        char method[16], path[512];
        char resp[4096];
        int i, resplen;
        const ut_mock_route_t *route = NULL;

        if (ut_mock_read_headers(client_fd, reqbuf, sizeof(reqbuf)) < 0)
                return;

        if (sscanf(reqbuf, "%15s %511s ", method, path) != 2)
                return;

        for (i = 0; i < s->n_routes; i++) {
                if (!strcmp(s->routes[i].method, method) &&
                    !strcmp(s->routes[i].path, path)) {
                        route = &s->routes[i];
                        break;
                }
        }

        if (route) {
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
                send(client_fd, resp, (size_t)resplen, 0);
                if (route->body && route->body_len > 0)
                        send(client_fd, route->body, route->body_len, 0);
        } else {
                resplen = rd_snprintf(resp, sizeof(resp),
                                      "HTTP/1.1 404 Not Found\r\n"
                                      "Content-Length: 0\r\n"
                                      "Connection: close\r\n"
                                      "\r\n");
                send(client_fd, resp, (size_t)resplen, 0);
        }
}

static int ut_mock_server_thread(void *arg) {
        ut_mock_server_t *s = arg;
        while (!s->stop) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int client_fd =
                    accept(s->listen_fd, (struct sockaddr *)&caddr, &clen);
                if (client_fd < 0) {
                        /* Listen socket was closed to unblock accept()
                         * during destroy, or we were interrupted. */
                        if (s->stop || errno == EBADF || errno == EINVAL)
                                break;
                        continue;
                }
                ut_mock_handle_request(client_fd, s);
                close(client_fd);
        }
        return 0;
}

static ut_mock_server_t *ut_mock_server_new(void) {
        ut_mock_server_t *s;
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int opt        = 1;

        s            = rd_calloc(1, sizeof(*s));
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
        rd_free(s);
        return NULL;
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

#endif /* !_WIN32 (mock server uses POSIX sockets) */


int unittest_aws_credentials(void) {
        int fails = 0;

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
#endif

        return fails;
}

#endif /* WITH_CURL */
