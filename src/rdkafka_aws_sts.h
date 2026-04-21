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
 * @file rdkafka_aws_sts.h
 * @brief AWS STS client (internal).
 *
 * Thin wrapper around libcurl that issues SigV4-signed requests to the
 * regional STS endpoint. V1 covers `GetWebIdentityToken` (outbound OIDC
 * federation, Nov 2025 API). `AssumeRoleWithWebIdentity` (needed by M5's
 * EKS IRSA provider) will be added to this same file later.
 *
 * Signing is delegated to libcurl's CURLOPT_AWS_SIGV4 (added in 7.75.0).
 */

#ifndef _RDKAFKA_AWS_STS_H_
#define _RDKAFKA_AWS_STS_H_

#include "rdkafka_int.h"
#include "rdkafka_aws_credentials.h"


/**
 * @brief An AWS-signed JWT returned by sts:GetWebIdentityToken, with its
 *        absolute expiration (rd_clock()-basis microseconds; 0 means unknown).
 *
 * The JWT itself is opaque — librdkafka forwards it to the external OIDC
 * service via OAUTHBEARER SASL and does not inspect claims.
 */
typedef struct rd_kafka_aws_sts_jwt_s {
        char *token;
        rd_ts_t expiration_us;
} rd_kafka_aws_sts_jwt_t;

void rd_kafka_aws_sts_jwt_destroy(rd_kafka_aws_sts_jwt_t *jwt);


/**
 * @brief Call STS `GetWebIdentityToken` with SigV4 signing.
 *
 * Builds `https://sts.<region>.amazonaws.com/` (or the value of the
 * AWS_STS_ENDPOINT_URL env var if set — used by tests to redirect to an
 * in-process mock), signs with the supplied credentials, and returns the
 * JWT on success.
 *
 * @param rk                may be NULL for tests; used only for logging.
 * @param region            AWS region (required; GetWebIdentityToken is
 *                          not available on the STS global endpoint).
 * @param audience          Populates the JWT `aud` claim. Required.
 * @param signing_algorithm "RS256" or "ES384" (AWS-recommended).
 * @param duration_seconds  JWT lifetime; AWS range is 60..3600.
 * @param creds             AWS credentials to sign with; must be non-NULL.
 * @param jwtp              Out: on success, a newly allocated JWT struct
 *                          that the caller frees with
 *                          rd_kafka_aws_sts_jwt_destroy().
 * @param errstr            Populated on error.
 * @param errstr_size       Size of errstr buffer.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success. On error:
 *           - RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED if libcurl is too old
 *             to support CURLOPT_AWS_SIGV4 (< 7.75.0).
 *           - RD_KAFKA_RESP_ERR__AUTHENTICATION on STS rejection
 *             (AccessDenied / ExpiredToken / missing permission / etc).
 *           - RD_KAFKA_RESP_ERR__TRANSPORT on network error.
 *           - RD_KAFKA_RESP_ERR__INVALID_ARG on unparseable response.
 */
rd_kafka_resp_err_t
rd_kafka_aws_sts_get_web_identity_token(rd_kafka_t *rk,
                                        const char *region,
                                        const char *audience,
                                        const char *signing_algorithm,
                                        int duration_seconds,
                                        const rd_kafka_aws_credentials_t *creds,
                                        rd_kafka_aws_sts_jwt_t **jwtp,
                                        char *errstr,
                                        size_t errstr_size);


/**
 * @brief Call STS `AssumeRoleWithWebIdentity` — unauthenticated.
 *
 * Exchanges a web-identity JWT (e.g., an EKS IRSA projected service-account
 * token) for AWS temporary credentials. This is the inverse of
 * GetWebIdentityToken: here AWS is the relying party that verifies an
 * externally-issued JWT against a configured OIDC trust and returns AWS
 * credentials.
 *
 * **Unauthenticated**: no SigV4 signature, no Authorization header. The JWT
 * itself is the caller's proof of identity. Confirmed on the wire in Probe
 * C (`auth_type: none`).
 *
 * @param rk                 may be NULL for tests.
 * @param region             AWS region (e.g. "eu-north-1"). Required —
 *                           librdkafka never auto-defaults.
 * @param role_arn           IAM role ARN to assume (AWS_ROLE_ARN).
 * @param role_session_name  Required tag for this session; callers typically
 *                           generate e.g. "librdkafka-<timestamp>".
 * @param web_identity_token The JWT (whitespace already stripped by caller
 *                           if read from a file).
 * @param credsp             Out: newly allocated credentials on success.
 * @param errstr             Populated on error.
 * @param errstr_size        Size of errstr buffer.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success. On error:
 *           - RD_KAFKA_RESP_ERR__AUTHENTICATION for STS rejections
 *             (InvalidIdentityToken, ExpiredToken, AccessDenied, ...).
 *           - RD_KAFKA_RESP_ERR__TRANSPORT on network error.
 *           - RD_KAFKA_RESP_ERR__BAD_MSG on unparseable response.
 *           - RD_KAFKA_RESP_ERR__INVALID_ARG on bad inputs.
 */
rd_kafka_resp_err_t rd_kafka_aws_sts_assume_role_with_web_identity(
    rd_kafka_t *rk,
    const char *region,
    const char *role_arn,
    const char *role_session_name,
    const char *web_identity_token,
    rd_kafka_aws_credentials_t **credsp,
    char *errstr,
    size_t errstr_size);


#endif /* _RDKAFKA_AWS_STS_H_ */
