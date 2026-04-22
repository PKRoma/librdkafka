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

#ifndef _RDKAFKA_OAUTHBEARER_AWS_CONFIG_H_
#define _RDKAFKA_OAUTHBEARER_AWS_CONFIG_H_

#include <stddef.h>

#include "rdkafka.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare the STS provider handle so we can hold a pointer to
 * it without dragging aws_sts_provider.h (and transitively aws-sdk-cpp
 * headers) into this pure-C data header. */
struct rd_kafka_aws_sts_provider_s;

/**
 * @brief Parsed AWS OAUTHBEARER plugin configuration plus per-client
 *        runtime state.
 *
 * Lifecycle:
 *   - Allocated at conf_init() / rd_kafka_oauthbearer_aws_register() time.
 *   - Property fields populated incrementally by on_conf_set.
 *   - rk + provider + provider_error set at on_new() time if the conf is
 *     used to create a client. Remain NULL if the conf is destroyed
 *     without ever creating a client.
 *   - Freed at on_conf_destroy() time (by rd_kafka_oauthbearer_aws_conf_destroy).
 */
typedef struct rd_kafka_oauthbearer_aws_conf_s {
        /* User-facing properties (M2). */
        char *audience;          /**< required, no default */
        char *region;            /**< required, no default */
        char *signing_algorithm; /**< default "ES384"; values ES384|RS256 */
        int duration_seconds;    /**< default 300; range 60..3600 */
        char *sts_endpoint;      /**< optional; FIPS/VPC override */

        /* Runtime state (M4). Populated at on_new time; NULL otherwise. */
        rd_kafka_t *rk; /**< the client this ctx is bound to */
        struct rd_kafka_aws_sts_provider_s
            *provider;       /**< STS provider, NULL if init failed */
        char *provider_error; /**< init error message; NULL on success */
} rd_kafka_oauthbearer_aws_conf_t;

rd_kafka_oauthbearer_aws_conf_t *rd_kafka_oauthbearer_aws_conf_new(void);

void rd_kafka_oauthbearer_aws_conf_destroy(
    rd_kafka_oauthbearer_aws_conf_t *c);

/**
 * @brief Attempt to handle a configuration property.
 *
 * @returns RD_KAFKA_CONF_OK      — property is in our namespace and was
 *                                  accepted.
 * @returns RD_KAFKA_CONF_UNKNOWN — property is not in our namespace;
 *                                  caller should pass through to librdkafka
 *                                  core or other interceptors.
 * @returns RD_KAFKA_CONF_INVALID — property is in our namespace but the
 *                                  value (or property name itself) is
 *                                  invalid; `errstr` is populated.
 */
rd_kafka_conf_res_t rd_kafka_oauthbearer_aws_conf_set(
    rd_kafka_oauthbearer_aws_conf_t *c,
    const char *name,
    const char *val,
    char *errstr,
    size_t errstr_size);

/**
 * @brief Validate that all required properties are set.
 *
 * @returns 0 on success.
 * @returns -1 on failure, with a human-readable message in `errstr`.
 */
int rd_kafka_oauthbearer_aws_conf_validate(
    const rd_kafka_oauthbearer_aws_conf_t *c,
    char *errstr,
    size_t errstr_size);

#ifdef __cplusplus
}
#endif

#endif /* _RDKAFKA_OAUTHBEARER_AWS_CONFIG_H_ */
