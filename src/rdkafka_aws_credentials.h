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
 * @file rdkafka_aws_credentials.h
 * @brief AWS credential types and provider chain (internal).
 *
 * Used by the OAUTHBEARER AWS STS GetWebIdentityToken path to resolve
 * AWS credentials from the standard SDK sources (env, web identity,
 * container, IMDS) in a chained fashion.
 */

#ifndef _RDKAFKA_AWS_CREDENTIALS_H_
#define _RDKAFKA_AWS_CREDENTIALS_H_

#include "rdkafka_int.h"

/**
 * @brief A set of AWS credentials (refcounted, immutable after construction).
 *
 * Session token is optional and NULL for long-lived IAM-user credentials.
 * Expiration is an absolute time in the rd_clock() basis (microseconds since
 * some monotonic epoch); 0 means "never expires".
 */
typedef struct rd_kafka_aws_credentials_s {
        char *access_key_id;
        char *secret_access_key;
        char *session_token;
        rd_ts_t expiration_us;
        rd_refcnt_t refcnt;
} rd_kafka_aws_credentials_t;


/**
 * @brief Construct a new credentials object.
 *
 * All string arguments are deep-copied. \p session_token and \p expiration_us
 * may be NULL/0 for long-lived credentials.
 *
 * @returns a new credentials object with refcount 1. Free with
 *          rd_kafka_aws_credentials_destroy().
 */
rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_new(const char *access_key_id,
                             const char *secret_access_key,
                             const char *session_token,
                             rd_ts_t expiration_us);

/**
 * @brief Increment refcount, returning the same pointer.
 */
rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_keep(rd_kafka_aws_credentials_t *creds);

/**
 * @brief Decrement refcount; free when it reaches zero.
 */
void rd_kafka_aws_credentials_destroy(rd_kafka_aws_credentials_t *creds);

/**
 * @returns rd_true if \p creds has a non-zero expiration that is at or before
 *          \p now_us (rd_clock() basis).
 */
rd_bool_t
rd_kafka_aws_credentials_expired(const rd_kafka_aws_credentials_t *creds,
                                 rd_ts_t now_us);


/**
 * @brief Result of a provider's resolve() call.
 *
 * @c OK    - credentials were produced; \p *credsp is set (caller owns the
 * ref).
 * @c SKIP  - provider is not applicable here (e.g. env vars not set); chain
 *            should try the next provider.
 * @c FATAL - provider is applicable but failed in a way that the chain should
 *            surface to the caller rather than silently falling back.
 *            \p errstr is populated.
 *
 * The SKIP/FATAL distinction is load-bearing. Example: IMDS returning 404
 * during startup races is transient and should SKIP; IMDS returning 403 means
 * the instance role exists but is misconfigured, which is FATAL.
 */
typedef enum rd_kafka_aws_creds_result_e {
        RD_KAFKA_AWS_CREDS_OK    = 0,
        RD_KAFKA_AWS_CREDS_SKIP  = 1,
        RD_KAFKA_AWS_CREDS_FATAL = 2,
} rd_kafka_aws_creds_result_t;


typedef struct rd_kafka_aws_creds_provider_s rd_kafka_aws_creds_provider_t;

/**
 * @brief Provider resolve function type.
 *
 * @param self          The provider (accessor for opaque state).
 * @param credsp        On OK, receives a new credentials ref (caller owns).
 * @param errstr        On FATAL, receives a human-readable error.
 * @param errstr_size   Size of \p errstr buffer.
 */
typedef rd_kafka_aws_creds_result_t (*rd_kafka_aws_creds_resolve_fn)(
    rd_kafka_aws_creds_provider_t *self,
    rd_kafka_aws_credentials_t **credsp,
    char *errstr,
    size_t errstr_size);

/**
 * @brief Provider destroy function type.
 */
typedef void (*rd_kafka_aws_creds_destroy_fn)(
    rd_kafka_aws_creds_provider_t *self);


/**
 * @brief A credentials provider. Wraps one source of AWS credentials.
 *
 * Providers are composable: the `chain` provider walks an ordered list of
 * inner providers and returns the first OK; the `cached` provider (future
 * milestone) wraps another provider with an expiry-driven cache.
 */
struct rd_kafka_aws_creds_provider_s {
        const char *name; /* e.g. "env", "chain"; not owned */
        rd_kafka_aws_creds_resolve_fn resolve;
        rd_kafka_aws_creds_destroy_fn destroy;
        rd_kafka_t *rk; /* for logging; may be NULL in unit tests */
        void *opaque;   /* provider-specific */
};


/**
 * @brief Destroy a provider (and any nested providers it owns).
 */
void rd_kafka_aws_creds_provider_destroy(rd_kafka_aws_creds_provider_t *p);


/**
 * @brief Environment-variable-based credentials provider.
 *
 * Reads AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, and (optionally)
 * AWS_SESSION_TOKEN.
 *
 *  - Neither AKID nor SECRET set       -> SKIP
 *  - Only one of AKID/SECRET set       -> FATAL (misconfiguration)
 *  - Both set (optionally with token)  -> OK
 */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_env_new(rd_kafka_t *rk);


/**
 * @brief EC2 Instance Metadata Service v2 (IMDSv2) credentials provider.
 *
 * Two-call dance on the EC2 link-local endpoint:
 *   1. PUT /latest/api/token with X-aws-ec2-metadata-token-ttl-seconds header
 *   2. GET /latest/meta-data/iam/security-credentials/ (the role name)
 *   3. GET /latest/meta-data/iam/security-credentials/<role> (JSON creds)
 *
 * Honours the AWS SDK environment variables:
 *   - AWS_EC2_METADATA_DISABLED=true         -> immediate SKIP
 *   - AWS_EC2_METADATA_SERVICE_ENDPOINT      -> endpoint override (for tests
 *                                               and non-default deployments)
 *
 * Result semantics:
 *   - IMDS unreachable (connection refused, timeout) -> SKIP
 *   - IMDS reachable, no role attached               -> SKIP
 *   - IMDS reachable with role, but creds fetch/parse failed -> FATAL
 *
 * The provider maintains an internal session-token cache that is refreshed
 * when stale.
 */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_imds_new(rd_kafka_t *rk);


/**
 * @brief Chain of credentials providers; walks providers in order, returns
 *        first OK.
 *
 * Takes ownership of the provided providers — destroying the chain destroys
 * all inner providers. On FATAL from any inner provider, stops and returns
 * FATAL (does not silently try the next).
 *
 * @param providers   Array of provider pointers, takes ownership.
 * @param n_providers Number of entries in \p providers.
 */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_chain_new(rd_kafka_t *rk,
                                      rd_kafka_aws_creds_provider_t **providers,
                                      size_t n_providers);


/**
 * @brief Resolve credentials from a provider.
 *
 * Convenience wrapper that invokes the provider's resolve function.
 * See \ref rd_kafka_aws_creds_result_t for the return semantics.
 */
rd_kafka_aws_creds_result_t
rd_kafka_aws_creds_provider_resolve(rd_kafka_aws_creds_provider_t *p,
                                    rd_kafka_aws_credentials_t **credsp,
                                    char *errstr,
                                    size_t errstr_size);


/*
 * Shared helpers used by both rdkafka_aws_credentials.c and rdkafka_aws_sts.c.
 * Kept here rather than in a separate header because the two files form a
 * small tightly-coupled subsystem and an extra header would be overkill.
 */

/**
 * @brief Parse an ISO 8601 / RFC 3339 timestamp string to UTC epoch seconds.
 *
 * Accepts `Z`, `+HH:MM`, `+HHMM`, `-HH:MM` offset forms and optional
 * fractional seconds. Timezone designator is REQUIRED (strict form).
 *
 * @returns rd_true on success with \p *epoch_seconds set, else rd_false.
 */
rd_bool_t rd_aws_parse_iso8601_utc(const char *iso, int64_t *epoch_seconds);

/**
 * @brief Convert wall-clock Unix-epoch seconds to an rd_clock()-basis
 *        absolute microsecond timestamp. Clock-jump-safe because the
 *        delta is computed at call time.
 *
 * @returns rd_clock() if the input is already in the past, else the
 *          absolute monotonic us value.
 */
rd_ts_t rd_aws_epoch_to_monotonic_us(int64_t exp_epoch);


#if WITH_CURL
int unittest_aws_credentials(void);
#endif

#endif /* _RDKAFKA_AWS_CREDENTIALS_H_ */
