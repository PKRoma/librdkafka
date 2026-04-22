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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "aws_sts_provider.h"

static const char AWS_PROP_PREFIX[]       = "sasl.oauthbearer.aws.";
static const size_t AWS_PROP_PREFIX_LEN   = sizeof(AWS_PROP_PREFIX) - 1;

static const char DEFAULT_SIGNING_ALGO[]  = "ES384";
static const int DEFAULT_DURATION_SECONDS = 300;
static const int MIN_DURATION_SECONDS     = 60;
static const int MAX_DURATION_SECONDS     = 3600;

static char *my_strdup(const char *s) {
        size_t n;
        char *p;
        if (!s)
                return NULL;
        n = strlen(s) + 1;
        p = (char *)malloc(n);
        if (p)
                memcpy(p, s, n);
        return p;
}

static void replace_str(char **dst, const char *val) {
        char *tmp = val ? my_strdup(val) : NULL;
        free(*dst);
        *dst = tmp;
}

rd_kafka_oauthbearer_aws_conf_t *rd_kafka_oauthbearer_aws_conf_new(void) {
        rd_kafka_oauthbearer_aws_conf_t *c;
        c = (rd_kafka_oauthbearer_aws_conf_t *)calloc(1, sizeof(*c));
        if (!c)
                return NULL;
        c->signing_algorithm = my_strdup(DEFAULT_SIGNING_ALGO);
        c->duration_seconds  = DEFAULT_DURATION_SECONDS;
        return c;
}

void rd_kafka_oauthbearer_aws_conf_destroy(
    rd_kafka_oauthbearer_aws_conf_t *c) {
        if (!c)
                return;
        free(c->audience);
        free(c->region);
        free(c->signing_algorithm);
        free(c->sts_endpoint);
        /* Runtime state (M4). provider is NULL if on_new never fired or
         * if provider construction failed. */
        if (c->provider)
                rd_kafka_aws_sts_provider_destroy(c->provider);
        free(c->provider_error);
        free(c);
}

rd_kafka_conf_res_t rd_kafka_oauthbearer_aws_conf_set(
    rd_kafka_oauthbearer_aws_conf_t *c,
    const char *name,
    const char *val,
    char *errstr,
    size_t errstr_size) {
        const char *sub;

        if (strncmp(name, AWS_PROP_PREFIX, AWS_PROP_PREFIX_LEN) != 0)
                return RD_KAFKA_CONF_UNKNOWN;

        sub = name + AWS_PROP_PREFIX_LEN;

        if (!strcmp(sub, "audience")) {
                if (!val || !*val) {
                        snprintf(errstr, errstr_size,
                                 "%s: value must be a non-empty string",
                                 name);
                        return RD_KAFKA_CONF_INVALID;
                }
                replace_str(&c->audience, val);
                return RD_KAFKA_CONF_OK;
        }

        if (!strcmp(sub, "region")) {
                if (!val || !*val) {
                        snprintf(errstr, errstr_size,
                                 "%s: value must be a non-empty string",
                                 name);
                        return RD_KAFKA_CONF_INVALID;
                }
                replace_str(&c->region, val);
                return RD_KAFKA_CONF_OK;
        }

        if (!strcmp(sub, "signing.algorithm")) {
                if (val && *val && strcmp(val, "ES384") != 0 &&
                    strcmp(val, "RS256") != 0) {
                        snprintf(errstr, errstr_size,
                                 "%s: must be \"ES384\" or \"RS256\" "
                                 "(got \"%s\")",
                                 name, val);
                        return RD_KAFKA_CONF_INVALID;
                }
                replace_str(&c->signing_algorithm,
                            (val && *val) ? val : DEFAULT_SIGNING_ALGO);
                return RD_KAFKA_CONF_OK;
        }

        if (!strcmp(sub, "duration.seconds")) {
                char *endptr = NULL;
                long v;
                if (!val || !*val) {
                        c->duration_seconds = DEFAULT_DURATION_SECONDS;
                        return RD_KAFKA_CONF_OK;
                }
                v = strtol(val, &endptr, 10);
                if (!endptr || *endptr != '\0' ||
                    v < (long)MIN_DURATION_SECONDS ||
                    v > (long)MAX_DURATION_SECONDS) {
                        snprintf(errstr, errstr_size,
                                 "%s: must be an integer in [%d, %d] "
                                 "(got \"%s\")",
                                 name, MIN_DURATION_SECONDS,
                                 MAX_DURATION_SECONDS, val);
                        return RD_KAFKA_CONF_INVALID;
                }
                c->duration_seconds = (int)v;
                return RD_KAFKA_CONF_OK;
        }

        if (!strcmp(sub, "sts.endpoint")) {
                if (val && *val && strncmp(val, "https://", 8) != 0) {
                        snprintf(errstr, errstr_size,
                                 "%s: must start with \"https://\" "
                                 "(got \"%s\")",
                                 name, val);
                        return RD_KAFKA_CONF_INVALID;
                }
                replace_str(&c->sts_endpoint,
                            (val && *val) ? val : NULL);
                return RD_KAFKA_CONF_OK;
        }

        /* Property is in our namespace but not a known name. Likely a
         * typo — fail loud rather than silently passing through, since
         * otherwise the app would see no effect and spend time debugging. */
        snprintf(errstr, errstr_size,
                 "%s: unknown AWS OAUTHBEARER configuration property", name);
        return RD_KAFKA_CONF_INVALID;
}

int rd_kafka_oauthbearer_aws_conf_validate(
    const rd_kafka_oauthbearer_aws_conf_t *c,
    char *errstr,
    size_t errstr_size) {
        if (!c->audience || !*c->audience) {
                snprintf(errstr, errstr_size,
                         "sasl.oauthbearer.aws.audience is required but "
                         "was not set");
                return -1;
        }
        if (!c->region || !*c->region) {
                snprintf(errstr, errstr_size,
                         "sasl.oauthbearer.aws.region is required but "
                         "was not set");
                return -1;
        }
        return 0;
}
