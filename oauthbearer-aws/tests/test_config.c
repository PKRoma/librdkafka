/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * BSD-2-Clause (see file header in project sources).
 */

/**
 * @brief Unit tests for config.c property parser + validator.
 *
 * Exercises the same matrix as the M2 smoke test but via CTest, and
 * without needing a running plugin / librdkafka client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define CHECK(cond, msg)                                                       \
        do {                                                                   \
                if (!(cond)) {                                                 \
                        fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__,        \
                                __LINE__, msg);                                \
                        exit(1);                                               \
                }                                                              \
        } while (0)

int main(void) {
        char errstr[512];
        rd_kafka_oauthbearer_aws_conf_t *c;
        rd_kafka_conf_res_t r;

        printf("test_config:\n");

        /* 1. All five properties accept valid values. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        CHECK(c, "conf_new");

        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.audience", "https://example.com", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_OK, "audience accepted");

        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.region", "eu-north-1", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_OK, "region accepted");

        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.signing.algorithm", "RS256", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_OK, "signing.algorithm=RS256 accepted");
        CHECK(strcmp(c->signing_algorithm, "RS256") == 0,
              "signing.algorithm stored");

        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.duration.seconds", "600", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_OK, "duration.seconds=600 accepted");
        CHECK(c->duration_seconds == 600, "duration parsed");

        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.sts.endpoint",
            "https://sts-fips.eu-north-1.amazonaws.com", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_OK, "sts.endpoint accepted");

        CHECK(rd_kafka_oauthbearer_aws_conf_validate(c, errstr,
                                                    sizeof(errstr)) == 0,
              "validation passes with all required set");

        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  all_valid_props OK\n");

        /* 2. Duration out of range rejected. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.duration.seconds", "5000", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_INVALID, "5000 rejected");
        CHECK(strstr(errstr, "3600") != NULL, "errstr mentions 3600 ceiling");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  duration_out_of_range OK\n");

        /* 3. Invalid signing.algorithm rejected. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.signing.algorithm", "HS256", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_INVALID, "HS256 rejected");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  bad_signing_algo OK\n");

        /* 4. Typo in AWS namespace rejected as unknown. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.audiance", "foo", errstr,
            sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_INVALID, "typo rejected");
        CHECK(strstr(errstr, "unknown") != NULL, "errstr says unknown");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  typo_rejected OK\n");

        /* 5. Unrelated property passes through as UNKNOWN (librdkafka-side). */
        c = rd_kafka_oauthbearer_aws_conf_new();
        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "bootstrap.servers", "localhost:9092", errstr, sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_UNKNOWN,
              "non-AWS prop passes through as UNKNOWN");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  unrelated_prop_passthrough OK\n");

        /* 6. Validation fails when required fields missing. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        CHECK(rd_kafka_oauthbearer_aws_conf_validate(c, errstr,
                                                    sizeof(errstr)) != 0,
              "validate rejects empty ctx");
        CHECK(strstr(errstr, "audience") != NULL,
              "errstr names missing audience");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  missing_required OK\n");

        /* 7. sts.endpoint requires https:// prefix. */
        c = rd_kafka_oauthbearer_aws_conf_new();
        r = rd_kafka_oauthbearer_aws_conf_set(
            c, "sasl.oauthbearer.aws.sts.endpoint",
            "http://insecure.example.com", errstr, sizeof(errstr));
        CHECK(r == RD_KAFKA_CONF_INVALID, "http:// rejected");
        CHECK(strstr(errstr, "https") != NULL, "errstr mentions https");
        rd_kafka_oauthbearer_aws_conf_destroy(c);
        printf("  http_endpoint_rejected OK\n");

        /* 8. destroy(NULL) is safe. */
        rd_kafka_oauthbearer_aws_conf_destroy(NULL);
        printf("  destroy_null_safe OK\n");

        printf("all test_config cases passed\n");
        return 0;
}
