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

#ifndef _RDKAFKA_OAUTHBEARER_AWS_H_
#define _RDKAFKA_OAUTHBEARER_AWS_H_

#include "rdkafka.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief librdkafka plugin ABI entry point.
 *
 * Invoked by librdkafka's plugin loader (src/rdkafka_plugin.c) when the
 * user sets plugin.library.paths to include this plugin. Applications
 * statically linking librdkafka should call
 * rd_kafka_oauthbearer_aws_register() instead.
 */
rd_kafka_resp_err_t conf_init(rd_kafka_conf_t *conf,
                              void **plug_opaquep,
                              char *errstr,
                              size_t errstr_size);

/**
 * @brief Explicit registration for static-librdkafka consumers.
 *
 * Equivalent to setting plugin.library.paths, for applications that
 * statically link librdkafka (e.g. confluent-kafka-go, distroless
 * containers) and cannot or do not want to rely on dlopen.
 */
rd_kafka_resp_err_t
rd_kafka_oauthbearer_aws_register(rd_kafka_conf_t *conf,
                                  char *errstr,
                                  size_t errstr_size);

#ifdef __cplusplus
}
#endif

#endif /* _RDKAFKA_OAUTHBEARER_AWS_H_ */
