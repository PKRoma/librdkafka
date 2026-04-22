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

#ifndef _RDKAFKA_AWS_STS_PROVIDER_H_
#define _RDKAFKA_AWS_STS_PROVIDER_H_

#include <stddef.h>
#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle wrapping an aws-sdk-cpp STSClient bound to the
 *        plugin's parsed configuration (audience, region, signing algo,
 *        duration, optional sts endpoint override).
 *
 * One provider per rd_kafka_conf_t / rd_kafka_t. Thread-safe for
 * concurrent get_token() calls — STSClient itself is thread-safe per
 * aws-sdk-cpp docs — though in practice librdkafka serialises refresh
 * callback invocations per client.
 */
typedef struct rd_kafka_aws_sts_provider_s rd_kafka_aws_sts_provider_t;

/**
 * @brief Construct a provider from parsed config.
 *
 * Takes a deep-copy of the configuration values it needs; the caller's
 * config struct may be destroyed or mutated independently after return.
 *
 * @returns new provider on success; NULL on failure (errstr populated).
 */
rd_kafka_aws_sts_provider_t *rd_kafka_aws_sts_provider_new(
    const rd_kafka_oauthbearer_aws_conf_t *conf,
    char *errstr,
    size_t errstr_size);

/**
 * @brief Destroy a provider. Safe to call with NULL.
 */
void rd_kafka_aws_sts_provider_destroy(rd_kafka_aws_sts_provider_t *p);

/**
 * @brief Synchronously mint a fresh JWT via AWS STS GetWebIdentityToken.
 *
 * Bridges aws-sdk-cpp's async callable to a blocking call via future::get().
 * Safe to invoke from librdkafka's background refresh thread (no
 * SynchronizationContext; no deadlock).
 *
 * On success: *token_out is a malloc'd NUL-terminated string the caller
 * must free(); *expiration_ms_out is the token's expiration as
 * milliseconds since Unix epoch; returns 0.
 *
 * On failure: returns -1, errstr populated with a descriptive message
 * including the AWS error name and AWS-reported message where available.
 * *token_out and *expiration_ms_out are not touched.
 */
int rd_kafka_aws_sts_provider_get_token(rd_kafka_aws_sts_provider_t *p,
                                         char **token_out,
                                         int64_t *expiration_ms_out,
                                         char *errstr,
                                         size_t errstr_size);

#ifdef __cplusplus
}
#endif

#endif /* _RDKAFKA_AWS_STS_PROVIDER_H_ */
