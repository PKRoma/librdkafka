/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * BSD-2-Clause (see file header in project sources).
 */

/**
 * @brief M9 — real-AWS validation test (env-gated).
 *
 * Opt-in via RD_UT_AWS_PLUGIN=1. When the gate is unset, the test
 * prints a skip message and exits 0 so CTest stays green on dev
 * laptops / laptop-CI that doesn't have AWS creds.
 *
 * When enabled, the test:
 *   1. Constructs the provider with caller-supplied audience + region
 *      (RD_UT_AWS_AUDIENCE, RD_UT_AWS_REGION).
 *   2. Calls GetToken, expecting aws-sdk-cpp's DefaultChain to pick up
 *      IMDS / env / profile credentials.
 *   3. Validates the JWT shape: three '.'-separated base64url segments,
 *      non-empty, expiration in the future.
 *   4. Prints JWT length + expiration skew — useful for cross-client
 *      parity against the Go / .NET / native-C implementations
 *      (see project_aws_outbound_federation.md + sibling-language
 *      memos for reference byte-lengths on matched inputs).
 *
 * Intended to run on an EC2 host bound to an IAM role with
 * sts:GetWebIdentityToken allowed, and with outbound identity
 * federation enabled on the account. The same role/audience/region
 * the native-path M7 validation used — eu-north-1, role
 * ktrue-iam-sts-test-role — is the recommended starting point for
 * cross-client parity comparisons.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aws_sts_provider.h"
#include "config.h"

static int count_char(const char *s, char c) {
        int n = 0;
        for (; *s; s++)
                if (*s == c)
                        n++;
        return n;
}

int main(void) {
        const char *gate     = getenv("RD_UT_AWS_PLUGIN");
        const char *audience = getenv("RD_UT_AWS_AUDIENCE");
        const char *region   = getenv("RD_UT_AWS_REGION");
        const char *duration = getenv("RD_UT_AWS_DURATION_SECONDS");
        const char *algo     = getenv("RD_UT_AWS_SIGNING_ALGORITHM");

        if (!gate) {
                printf("test_real_aws: skipped (set RD_UT_AWS_PLUGIN=1 "
                       "plus RD_UT_AWS_AUDIENCE / RD_UT_AWS_REGION to run)\n");
                return 0;
        }

        if (!audience || !*audience || !region || !*region) {
                fprintf(stderr,
                        "test_real_aws: RD_UT_AWS_AUDIENCE and "
                        "RD_UT_AWS_REGION are required when "
                        "RD_UT_AWS_PLUGIN=1\n");
                return 1;
        }

        char errstr[512];
        rd_kafka_oauthbearer_aws_conf_t *conf;
        rd_kafka_aws_sts_provider_t *prov;

        conf = rd_kafka_oauthbearer_aws_conf_new();
        if (!conf) {
                fprintf(stderr, "conf_new OOM\n");
                return 2;
        }
        conf->audience = strdup(audience);
        conf->region   = strdup(region);
        if (duration && *duration)
                conf->duration_seconds = atoi(duration);
        if (algo && *algo) {
                free(conf->signing_algorithm);
                conf->signing_algorithm = strdup(algo);
        }

        prov =
            rd_kafka_aws_sts_provider_new(conf, errstr, sizeof(errstr));
        if (!prov) {
                fprintf(stderr, "provider_new failed: %s\n", errstr);
                rd_kafka_oauthbearer_aws_conf_destroy(conf);
                return 3;
        }

        char *token    = NULL;
        int64_t exp_ms = 0;
        time_t before  = time(NULL);

        if (rd_kafka_aws_sts_provider_get_token(prov, &token, &exp_ms,
                                                 errstr, sizeof(errstr)) !=
            0) {
                fprintf(stderr, "get_token failed: %s\n", errstr);
                rd_kafka_aws_sts_provider_destroy(prov);
                rd_kafka_oauthbearer_aws_conf_destroy(conf);
                return 4;
        }

        time_t now_s = time(NULL);
        int64_t exp_s_skew = exp_ms / 1000 - (int64_t)now_s;

        if (!token || !*token) {
                fprintf(stderr, "empty JWT returned\n");
                return 5;
        }

        size_t jwt_len = strlen(token);
        int dots       = count_char(token, '.');

        printf("test_real_aws: MINTED A JWT\n");
        printf("  audience:      %s\n", audience);
        printf("  region:        %s\n", region);
        printf("  duration_s:    %d\n", conf->duration_seconds);
        printf("  algorithm:     %s\n", conf->signing_algorithm);
        printf("  JWT length:    %zu bytes\n", jwt_len);
        printf("  JWT segments:  %d (header.payload.signature = 2 dots)\n",
               dots);
        printf("  exp (epoch-ms):%" PRId64 "\n", exp_ms);
        printf("  exp skew (s):  %+" PRId64 "  (%s)\n", exp_s_skew,
               exp_s_skew > 0 ? "future, good" : "PAST — failure");
        printf("  call latency:  %lld s\n",
               (long long)(now_s - before));

        int rc = 0;
        if (dots != 2) {
                fprintf(stderr, "FAIL: expected 2 dots in JWT, got %d\n",
                        dots);
                rc = 6;
        }
        if (exp_s_skew <= 0) {
                fprintf(stderr, "FAIL: expiration is not in the future\n");
                rc = 7;
        }
        if (jwt_len < 200) {
                /* AWS STS JWTs are ~1000-1500 bytes; <200 is almost
                 * certainly malformed. */
                fprintf(stderr,
                        "FAIL: JWT suspiciously short (%zu bytes)\n",
                        jwt_len);
                rc = 8;
        }

        if (rc == 0) {
                printf("test_real_aws: PASS\n");
                printf("  Cross-client parity note: Go / .NET recorded "
                       "1256 bytes; librdkafka native-path M7 recorded "
                       "1467 bytes. All on DIFFERENT inputs — compare "
                       "only against same audience / duration / algorithm "
                       "/ role on the same account.\n");
        }

        free(token);
        rd_kafka_aws_sts_provider_destroy(prov);
        rd_kafka_oauthbearer_aws_conf_destroy(conf);
        return rc;
}
