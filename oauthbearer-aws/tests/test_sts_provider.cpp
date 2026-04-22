/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2026 Confluent Inc.
 * All rights reserved.
 *
 * BSD-2-Clause (see file header in project sources).
 */

/**
 * @brief Unit tests for rdkafka_aws_sts_provider wired to a mock STS.
 *
 * Covers:
 *   happy_path        — mock returns a valid GetWebIdentityTokenResponse;
 *                       provider parses JWT + expiration; set_token path OK.
 *   error_response    — mock returns STS ErrorResponse XML;
 *                       errstr contains the AWS error code + message.
 *   empty_jwt         — mock returns success envelope with empty JWT;
 *                       provider rejects with a specific errstr.
 *   missing_required  — provider_new() fails without audience / region.
 *
 * Stubs the credential provider by setting fake env vars so aws-sdk-cpp's
 * DefaultAWSCredentialsProviderChain resolves without needing real IMDS /
 * config files. The signed request bytes never reach AWS; they go to the
 * local mock at 127.0.0.1:<random>.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "aws_sts_provider.h"
#include "config.h"
}

#include "mock_http_server.h"

#define CHECK(cond, msg)                                                       \
        do {                                                                   \
                if (!(cond)) {                                                 \
                        std::fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__,   \
                                     __LINE__, msg);                           \
                        std::exit(1);                                          \
                }                                                              \
        } while (0)

static std::string kHappyBody =
    "<?xml version=\"1.0\"?>"
    "<GetWebIdentityTokenResponse"
    " xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
    "<GetWebIdentityTokenResult>"
    "<WebIdentityToken>eyJhbGciOiJFUzM4NCIsInR5cCI6IkpXVCJ9.abc.def"
    "</WebIdentityToken>"
    "<Expiration>2030-04-22T20:30:00.000Z</Expiration>"
    "</GetWebIdentityTokenResult>"
    "<ResponseMetadata><RequestId>mock-id</RequestId></ResponseMetadata>"
    "</GetWebIdentityTokenResponse>";

static std::string kErrorBody =
    "<?xml version=\"1.0\"?>"
    "<ErrorResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
    "<Error><Type>Sender</Type>"
    "<Code>ValidationError</Code>"
    "<Message>Audience is invalid</Message></Error>"
    "<RequestId>mock-err-id</RequestId>"
    "</ErrorResponse>";

static std::string kEmptyJwtBody =
    "<?xml version=\"1.0\"?>"
    "<GetWebIdentityTokenResponse"
    " xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
    "<GetWebIdentityTokenResult>"
    "<WebIdentityToken></WebIdentityToken>"
    "<Expiration>2030-04-22T20:30:00.000Z</Expiration>"
    "</GetWebIdentityTokenResult>"
    "</GetWebIdentityTokenResponse>";

static rd_kafka_oauthbearer_aws_conf_t *make_conf(const std::string &endpoint) {
        auto *c = rd_kafka_oauthbearer_aws_conf_new();
        CHECK(c != nullptr, "conf_new");
        c->audience     = strdup("https://kafka.example.com");
        c->region       = strdup("eu-north-1");
        c->sts_endpoint = strdup(endpoint.c_str());
        return c;
}

static void test_happy_path() {
        MockHttpServer server;
        CHECK(server.Start(), "server start");
        server.SetResponse(200, "text/xml", kHappyBody);

        auto *conf = make_conf(server.Endpoint());
        char errstr[512]{};
        auto *p = rd_kafka_aws_sts_provider_new(conf, errstr, sizeof(errstr));
        CHECK(p != nullptr, errstr);

        char *token    = nullptr;
        int64_t exp_ms = 0;
        int ret = rd_kafka_aws_sts_provider_get_token(p, &token, &exp_ms,
                                                       errstr, sizeof(errstr));
        CHECK(ret == 0, errstr);
        CHECK(token != nullptr && std::strlen(token) > 0, "non-empty JWT");
        CHECK(std::string(token).find("eyJhbGc") == 0, "JWT matches mock");
        CHECK(exp_ms > 1'800'000'000'000LL, /* year 2027+ millis */
              "expiration parsed (year 2030 expected)");

        std::free(token);
        rd_kafka_aws_sts_provider_destroy(p);
        rd_kafka_oauthbearer_aws_conf_destroy(conf);
        std::printf("  happy_path OK (JWT %zu chars, exp_ms=%lld)\n",
                    std::strlen("eyJhbGciOiJFUzM4NCIsInR5cCI6IkpXVCJ9.abc.def"),
                    static_cast<long long>(exp_ms));
}

static void test_error_response() {
        MockHttpServer server;
        CHECK(server.Start(), "server start");
        server.SetResponse(400, "text/xml", kErrorBody);

        auto *conf = make_conf(server.Endpoint());
        char errstr[512]{};
        auto *p = rd_kafka_aws_sts_provider_new(conf, errstr, sizeof(errstr));
        CHECK(p != nullptr, errstr);

        char *token    = nullptr;
        int64_t exp_ms = 0;
        int ret = rd_kafka_aws_sts_provider_get_token(p, &token, &exp_ms,
                                                       errstr, sizeof(errstr));
        CHECK(ret != 0, "expected failure");
        CHECK(std::string(errstr).find("ValidationError") != std::string::npos,
              "errstr contains AWS error code");
        CHECK(std::string(errstr).find("Audience is invalid") !=
                  std::string::npos,
              "errstr contains AWS error message");

        rd_kafka_aws_sts_provider_destroy(p);
        rd_kafka_oauthbearer_aws_conf_destroy(conf);
        std::printf("  error_response OK (errstr=\"%s\")\n", errstr);
}

static void test_empty_jwt() {
        MockHttpServer server;
        CHECK(server.Start(), "server start");
        server.SetResponse(200, "text/xml", kEmptyJwtBody);

        auto *conf = make_conf(server.Endpoint());
        char errstr[512]{};
        auto *p = rd_kafka_aws_sts_provider_new(conf, errstr, sizeof(errstr));
        CHECK(p != nullptr, errstr);

        char *token    = nullptr;
        int64_t exp_ms = 0;
        int ret = rd_kafka_aws_sts_provider_get_token(p, &token, &exp_ms,
                                                       errstr, sizeof(errstr));
        CHECK(ret != 0, "expected failure");
        CHECK(std::string(errstr).find("empty") != std::string::npos ||
                  std::string(errstr).find("Empty") != std::string::npos ||
                  std::string(errstr).find("WebIdentityToken") !=
                      std::string::npos,
              "errstr mentions the empty-token condition");

        rd_kafka_aws_sts_provider_destroy(p);
        rd_kafka_oauthbearer_aws_conf_destroy(conf);
        std::printf("  empty_jwt OK (errstr=\"%s\")\n", errstr);
}

static void test_missing_required() {
        auto *conf = rd_kafka_oauthbearer_aws_conf_new();
        /* audience/region both unset */
        char errstr[512]{};
        auto *p = rd_kafka_aws_sts_provider_new(conf, errstr, sizeof(errstr));
        CHECK(p == nullptr, "provider_new should reject");
        CHECK(std::string(errstr).find("audience") != std::string::npos ||
                  std::string(errstr).find("region") != std::string::npos,
              "errstr names the missing field");
        rd_kafka_oauthbearer_aws_conf_destroy(conf);
        std::printf("  missing_required OK (errstr=\"%s\")\n", errstr);
}

int main() {
        /* Provide fake credentials so aws-sdk-cpp's DefaultChain resolves.
         * The SigV4 signature will be computed against these, sent to our
         * mock server, and ignored (the mock returns the canned response
         * regardless of signature validity). */
        setenv("AWS_ACCESS_KEY_ID", "AKIAFAKETEST0000NOOP", 1);
        setenv("AWS_SECRET_ACCESS_KEY",
               "FakeSecretKey0000000000000000000000000000", 1);
        /* Also unset AWS_PROFILE and disable IMDS probing so the chain
         * reliably picks the env provider — profile lookups in a test
         * environment can hit real ~/.aws/credentials. */
        unsetenv("AWS_PROFILE");
        setenv("AWS_EC2_METADATA_DISABLED", "true", 1);

        std::printf("test_sts_provider:\n");
        test_happy_path();
        test_error_response();
        test_empty_jwt();
        test_missing_required();
        std::printf("all test_sts_provider cases passed\n");
        return 0;
}
