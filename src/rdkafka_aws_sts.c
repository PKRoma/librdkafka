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
 * @file rdkafka_aws_sts.c
 * @brief AWS STS client (Milestone 7 of V1).
 *        See DESIGN_AWS_OAUTHBEARER_V1.md at the repo root.
 *
 * SigV4 signing is delegated to libcurl's CURLOPT_AWS_SIGV4 option
 * (added in 7.75.0). Probe A (Phase 0) validated that libcurl correctly
 * canonicalises manually-added X-Amz-Security-Token into the SignedHeaders
 * list, so we can pass temp credentials without having to sign by hand.
 */

#include "rdkafka_int.h"
#include "rdkafka_aws_credentials.h"
#include "rdkafka_aws_sts.h"

#include <curl/curl.h>
#include "rdhttp.h"


/* libcurl >= 7.75.0 is required for CURLOPT_AWS_SIGV4. Older versions
 * compile the file but the function returns NOT_IMPLEMENTED at runtime
 * with a clear message, so the rest of librdkafka keeps building on
 * older distros. */
#define RD_AWS_MIN_LIBCURL_VERSION_NUM 0x074b00 /* 7.75.0 */


/*
 * ============================================================
 * Tiny XML element extractor
 * ============================================================
 *
 * STS responses are classic SOAP-style XML with a flat nested structure
 * and no namespaces-on-elements, e.g.:
 *
 *   <GetWebIdentityTokenResponse ...>
 *     <GetWebIdentityTokenResult>
 *       <WebIdentityToken>...JWT...</WebIdentityToken>
 *       <Expiration>2026-04-21T...Z</Expiration>
 *     </GetWebIdentityTokenResult>
 *   </GetWebIdentityTokenResponse>
 *
 * and errors like:
 *
 *   <ErrorResponse ...>
 *     <Error>
 *       <Type>Sender</Type>
 *       <Code>AccessDenied</Code>
 *       <Message>...</Message>
 *     </Error>
 *     <RequestId>...</RequestId>
 *   </ErrorResponse>
 *
 * The tag names we care about (WebIdentityToken, Expiration, Code, Message)
 * appear exactly once per document, so a first-match substring scan is
 * sufficient and doesn't need a real XML parser.
 */

/**
 * @brief Extract the text content between `<tag>` and `</tag>` from \p xml.
 *
 * @returns newly allocated NUL-terminated string (caller frees with rd_free),
 *          or NULL if the tag was not found or is malformed.
 */
static char *rd_aws_xml_extract(const char *xml, const char *tag) {
        char open_tag[64];
        char close_tag[64];
        const char *start;
        const char *end;
        char *result;
        size_t len;

        rd_snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
        rd_snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

        start = strstr(xml, open_tag);
        if (!start)
                return NULL;
        start += strlen(open_tag);

        end = strstr(start, close_tag);
        if (!end)
                return NULL;

        len    = (size_t)(end - start);
        result = rd_malloc(len + 1);
        memcpy(result, start, len);
        result[len] = '\0';
        return result;
}


/*
 * ============================================================
 * JWT struct lifecycle
 * ============================================================
 */

void rd_kafka_aws_sts_jwt_destroy(rd_kafka_aws_sts_jwt_t *jwt) {
        if (!jwt)
                return;
        if (jwt->token) {
                /* Best-effort scrub — the JWT itself isn't a secret but
                 * is bearer authentication material. */
                memset(jwt->token, 0, strlen(jwt->token));
                rd_free(jwt->token);
        }
        rd_free(jwt);
}


/*
 * ============================================================
 * GetWebIdentityToken
 * ============================================================
 */

/**
 * @brief Build the STS endpoint URL.
 *
 * Prefers the AWS_STS_ENDPOINT_URL env var (for test redirect and VPC /
 * FIPS endpoints), falls back to the regional public endpoint. There is
 * no valid global endpoint for GetWebIdentityToken.
 */
static char *build_sts_endpoint_url(const char *region) {
        const char *override;
        char *url;
        size_t url_size;

        override = rd_getenv("AWS_STS_ENDPOINT_URL", NULL);
        if (override && *override)
                return rd_strdup(override);

        url_size = strlen(region) + 64;
        url      = rd_malloc(url_size);
        rd_snprintf(url, url_size, "https://sts.%s.amazonaws.com/", region);
        return url;
}

/**
 * @brief Build the form-urlencoded request body.
 *
 * Uses libcurl's curl_easy_escape() on the audience value since it can
 * contain URL-reserved characters (":" and "/") that must be percent-encoded.
 * DurationSeconds and SigningAlgorithm are constrained by AWS and don't
 * need escaping.
 */
static char *build_request_body(CURL *curl,
                                const char *audience,
                                const char *signing_algorithm,
                                int duration_seconds) {
        char *escaped_audience;
        char *body;
        size_t body_size;

        escaped_audience =
            curl_easy_escape(curl, audience, (int)strlen(audience));
        if (!escaped_audience)
                return NULL;

        body_size = strlen(escaped_audience) + strlen(signing_algorithm) + 128;
        body      = rd_malloc(body_size);
        rd_snprintf(body, body_size,
                    "Action=GetWebIdentityToken"
                    "&Version=2011-06-15"
                    "&Audience.member.1=%s"
                    "&SigningAlgorithm=%s"
                    "&DurationSeconds=%d",
                    escaped_audience, signing_algorithm, duration_seconds);
        curl_free(escaped_audience);
        return body;
}

/**
 * @brief Read the full response body out of \p hreq into a NUL-terminated C
 *        string. Caller frees with rd_free.
 */
static char *read_response_body(rd_http_req_t *hreq) {
        rd_slice_t slice;
        size_t len;
        char *buf;

        len = rd_buf_len(hreq->hreq_buf);
        buf = rd_malloc(len + 1);
        if (len > 0) {
                rd_slice_init_full(&slice, hreq->hreq_buf);
                rd_slice_read(&slice, buf, len);
        }
        buf[len] = '\0';
        return buf;
}

/**
 * @brief Format a helpful error message from an STS error XML body.
 *
 * STS error responses always have <Code> and usually <Message> inside
 * <ErrorResponse><Error>. If both are present we surface them; otherwise
 * fall back to the raw body.
 */
static void format_sts_error(int http_code,
                             const char *xml_body,
                             char *errstr,
                             size_t errstr_size) {
        char *err_code = rd_aws_xml_extract(xml_body, "Code");
        char *err_msg  = rd_aws_xml_extract(xml_body, "Message");

        if (err_code && err_msg)
                rd_snprintf(errstr, errstr_size,
                            "STS GetWebIdentityToken failed (HTTP %d): "
                            "%s: %s",
                            http_code, err_code, err_msg);
        else if (err_code)
                rd_snprintf(errstr, errstr_size,
                            "STS GetWebIdentityToken failed (HTTP %d): %s",
                            http_code, err_code);
        else
                rd_snprintf(errstr, errstr_size,
                            "STS GetWebIdentityToken failed (HTTP %d)",
                            http_code);

        RD_IF_FREE(err_code, rd_free);
        RD_IF_FREE(err_msg, rd_free);
}


rd_kafka_resp_err_t
rd_kafka_aws_sts_get_web_identity_token(rd_kafka_t *rk,
                                        const char *region,
                                        const char *audience,
                                        const char *signing_algorithm,
                                        int duration_seconds,
                                        const rd_kafka_aws_credentials_t *creds,
                                        rd_kafka_aws_sts_jwt_t **jwtp,
                                        char *errstr,
                                        size_t errstr_size) {
        rd_http_req_t hreq;
        rd_http_error_t *herr      = NULL;
        struct curl_slist *headers = NULL;
        char *url                  = NULL;
        char *body                 = NULL;
        char *xml_body             = NULL;
        char *web_identity_token   = NULL;
        char *expiration_str       = NULL;
        char sigv4_provider[128];
        char userpwd[512];
        char *sec_token_header     = NULL;
        rd_kafka_resp_err_t result = RD_KAFKA_RESP_ERR__FAIL;
#if LIBCURL_VERSION_NUM >= RD_AWS_MIN_LIBCURL_VERSION_NUM
        CURLcode cc;
#endif
        int64_t exp_epoch = 0;
        rd_ts_t exp_us    = 0;

        if (errstr_size > 0)
                errstr[0] = '\0';
        *jwtp = NULL;

        /* Validate arguments. */
        if (!region || !*region || !audience || !*audience ||
            !signing_algorithm || !*signing_algorithm ||
            duration_seconds < 60 || duration_seconds > 3600 || !creds ||
            !creds->access_key_id || !creds->secret_access_key) {
                rd_snprintf(errstr, errstr_size,
                            "invalid arguments to "
                            "rd_kafka_aws_sts_get_web_identity_token");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        url  = build_sts_endpoint_url(region);
        herr = rd_http_req_init(rk, &hreq, url);
        if (herr) {
                rd_snprintf(errstr, errstr_size,
                            "failed to initialise HTTP request to %s: %s", url,
                            herr->errstr);
                rd_http_error_destroy(herr);
                rd_free(url);
                return RD_KAFKA_RESP_ERR__TRANSPORT;
        }

#if LIBCURL_VERSION_NUM < RD_AWS_MIN_LIBCURL_VERSION_NUM
        /* Compile-time libcurl too old — graceful runtime failure. */
        rd_snprintf(errstr, errstr_size,
                    "AWS SigV4 signing requires libcurl >= 7.75.0 "
                    "(CURLOPT_AWS_SIGV4), but this build was linked "
                    "against libcurl %s",
                    LIBCURL_VERSION);
        result = RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
        goto cleanup;
#else
        /* Ask libcurl to SigV4-sign the request. Probe A proved this
         * handles X-Amz-Security-Token correctly when we add it via
         * CURLOPT_HTTPHEADER below. */
        rd_snprintf(sigv4_provider, sizeof(sigv4_provider), "aws:amz:%s:sts",
                    region);
        cc =
            curl_easy_setopt(hreq.hreq_curl, CURLOPT_AWS_SIGV4, sigv4_provider);
        if (cc != CURLE_OK) {
                /* Runtime libcurl is older than the build-time one. */
                rd_snprintf(errstr, errstr_size,
                            "libcurl at runtime does not support "
                            "CURLOPT_AWS_SIGV4 (need 7.75.0+, have %s): %s",
                            curl_version_info(CURLVERSION_NOW)->version,
                            curl_easy_strerror(cc));
                result = RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
                goto cleanup;
        }

        rd_snprintf(userpwd, sizeof(userpwd), "%s:%s", creds->access_key_id,
                    creds->secret_access_key);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_USERPWD, userpwd);

        /* Build request body. */
        body = build_request_body(hreq.hreq_curl, audience, signing_algorithm,
                                  duration_seconds);
        if (!body) {
                rd_snprintf(errstr, errstr_size,
                            "failed to URL-encode audience");
                result = RD_KAFKA_RESP_ERR__INVALID_ARG;
                goto cleanup;
        }

        /* Headers:
         *  - Content-Type is required and must be included in the signature;
         *    libcurl canonicalises all request headers so setting it via
         *    CURLOPT_HTTPHEADER is enough.
         *  - X-Amz-Security-Token for temporary credentials (session token);
         *    again libcurl includes it in SignedHeaders — validated in
         *    Probe A on a real EC2 instance.
         *  - Expect: (empty) disables libcurl's default "Expect: 100-continue"
         *    for POST/PUT with larger bodies, so we never pay its 1s wait.
         */
        headers = curl_slist_append(
            headers, "Content-Type: application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, "Expect:");
        if (creds->session_token && *creds->session_token) {
                size_t hlen      = strlen(creds->session_token) + 64;
                sec_token_header = rd_malloc(hlen);
                rd_snprintf(sec_token_header, hlen, "X-Amz-Security-Token: %s",
                            creds->session_token);
                headers = curl_slist_append(headers, sec_token_header);
        }
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_HTTPHEADER, headers);

        /* Actual POST. */
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(body));
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_TIMEOUT, 30);

        herr = rd_http_req_perform_sync(&hreq);

        /* Always read the body — we need it for both success parsing and
         * error classification. */
        xml_body = read_response_body(&hreq);

        if (herr) {
                int http_code = herr->code;
                format_sts_error(http_code, xml_body, errstr, errstr_size);
                rd_http_error_destroy(herr);
                herr = NULL;
                /* -1 is a transport error (DNS, connect, TLS, ...);
                 * everything else is an HTTP response from STS. */
                result = (http_code < 0) ? RD_KAFKA_RESP_ERR__TRANSPORT
                                         : RD_KAFKA_RESP_ERR__AUTHENTICATION;
                goto cleanup;
        }

        /* Success: parse <WebIdentityToken> and <Expiration>. */
        web_identity_token = rd_aws_xml_extract(xml_body, "WebIdentityToken");
        if (!web_identity_token || !*web_identity_token) {
                rd_snprintf(
                    errstr, errstr_size,
                    "STS GetWebIdentityToken: response missing "
                    "<WebIdentityToken> (HTTP 200 but unexpected body)");
                result = RD_KAFKA_RESP_ERR__BAD_MSG;
                goto cleanup;
        }

        expiration_str = rd_aws_xml_extract(xml_body, "Expiration");
        if (expiration_str && *expiration_str) {
                if (!rd_aws_parse_iso8601_utc(expiration_str, &exp_epoch)) {
                        rd_snprintf(errstr, errstr_size,
                                    "STS GetWebIdentityToken: malformed "
                                    "<Expiration>: %s",
                                    expiration_str);
                        result = RD_KAFKA_RESP_ERR__BAD_MSG;
                        goto cleanup;
                }
                exp_us = rd_aws_epoch_to_monotonic_us(exp_epoch);
        }

        *jwtp                  = rd_calloc(1, sizeof(**jwtp));
        (*jwtp)->token         = web_identity_token;
        (*jwtp)->expiration_us = exp_us;
        web_identity_token     = NULL; /* ownership transferred to *jwtp */
        result                 = RD_KAFKA_RESP_ERR_NO_ERROR;
#endif /* LIBCURL_VERSION_NUM >= 7.75.0 */

cleanup:
        RD_IF_FREE(herr, rd_http_error_destroy);
        RD_IF_FREE(headers, curl_slist_free_all);
        RD_IF_FREE(url, rd_free);
        RD_IF_FREE(body, rd_free);
        RD_IF_FREE(xml_body, rd_free);
        RD_IF_FREE(web_identity_token, rd_free);
        RD_IF_FREE(expiration_str, rd_free);
        RD_IF_FREE(sec_token_header, rd_free);
        rd_http_req_destroy(&hreq);
        return result;
}


/*
 * ============================================================
 * AssumeRoleWithWebIdentity
 * ============================================================
 *
 * Unauthenticated variant: no SigV4 signing, no Authorization header.
 * The web-identity token itself is the caller's proof of identity.
 *
 * Response body shape (live-confirmed in Probe C):
 *
 *   <AssumeRoleWithWebIdentityResponse ...>
 *     <AssumeRoleWithWebIdentityResult>
 *       <Credentials>
 *         <AccessKeyId>ASIA...</AccessKeyId>
 *         <SecretAccessKey>...</SecretAccessKey>
 *         <SessionToken>...</SessionToken>        ← note: NOT "Token"
 *         <Expiration>2026-04-21T...Z</Expiration>
 *       </Credentials>
 *       ... (SubjectFromWebIdentityToken, AssumedRoleUser, etc.) ...
 *     </AssumeRoleWithWebIdentityResult>
 *   </AssumeRoleWithWebIdentityResponse>
 *
 * Each tag we care about appears exactly once, so rd_aws_xml_extract()
 * works fine. Field names differ from the IMDS/ECS JSON shape
 * (SessionToken vs Token), so we extract and construct credentials
 * directly rather than reusing aws_creds_parse_json_response().
 */

rd_kafka_resp_err_t rd_kafka_aws_sts_assume_role_with_web_identity(
    rd_kafka_t *rk,
    const char *region,
    const char *role_arn,
    const char *role_session_name,
    const char *web_identity_token,
    rd_kafka_aws_credentials_t **credsp,
    char *errstr,
    size_t errstr_size) {
        rd_http_req_t hreq;
        rd_http_error_t *herr      = NULL;
        struct curl_slist *headers = NULL;
        char *url                  = NULL;
        char *body                 = NULL;
        char *xml_body             = NULL;
        char *escaped_role_arn     = NULL;
        char *escaped_session_name = NULL;
        char *escaped_token        = NULL;
        char *akid                 = NULL;
        char *secret               = NULL;
        char *session              = NULL;
        char *expiration_str       = NULL;
        rd_kafka_resp_err_t result = RD_KAFKA_RESP_ERR__FAIL;
        int64_t exp_epoch          = 0;
        rd_ts_t exp_us             = 0;
        size_t body_size;

        if (errstr_size > 0)
                errstr[0] = '\0';
        *credsp = NULL;

        if (!region || !*region || !role_arn || !*role_arn ||
            !role_session_name || !*role_session_name || !web_identity_token ||
            !*web_identity_token) {
                rd_snprintf(errstr, errstr_size,
                            "invalid arguments to "
                            "rd_kafka_aws_sts_assume_role_with_web_identity");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        url  = build_sts_endpoint_url(region);
        herr = rd_http_req_init(rk, &hreq, url);
        if (herr) {
                rd_snprintf(errstr, errstr_size,
                            "failed to initialise HTTP request to %s: %s", url,
                            herr->errstr);
                rd_http_error_destroy(herr);
                rd_free(url);
                return RD_KAFKA_RESP_ERR__TRANSPORT;
        }

        /* URL-encode every free-form input. RoleArn contains ":" and "/",
         * RoleSessionName can contain "@=+-,.", and the JWT is base64url
         * with "=" padding and "." separators — all must be percent-encoded
         * for the form body. */
        escaped_role_arn =
            curl_easy_escape(hreq.hreq_curl, role_arn, (int)strlen(role_arn));
        escaped_session_name = curl_easy_escape(
            hreq.hreq_curl, role_session_name, (int)strlen(role_session_name));
        escaped_token = curl_easy_escape(hreq.hreq_curl, web_identity_token,
                                         (int)strlen(web_identity_token));
        if (!escaped_role_arn || !escaped_session_name || !escaped_token) {
                rd_snprintf(errstr, errstr_size,
                            "failed to URL-encode AssumeRoleWithWebIdentity "
                            "arguments");
                result = RD_KAFKA_RESP_ERR__INVALID_ARG;
                goto cleanup;
        }

        body_size = strlen(escaped_role_arn) + strlen(escaped_session_name) +
                    strlen(escaped_token) + 128;
        body = rd_malloc(body_size);
        rd_snprintf(body, body_size,
                    "Action=AssumeRoleWithWebIdentity"
                    "&Version=2011-06-15"
                    "&RoleArn=%s"
                    "&RoleSessionName=%s"
                    "&WebIdentityToken=%s",
                    escaped_role_arn, escaped_session_name, escaped_token);

        /* No Authorization / X-Amz-Security-Token: this API is
         * unauthenticated by design. */
        headers = curl_slist_append(
            headers, "Content-Type: application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, "Expect:");
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(body));
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(hreq.hreq_curl, CURLOPT_TIMEOUT, 30);

        herr     = rd_http_req_perform_sync(&hreq);
        xml_body = read_response_body(&hreq);

        if (herr) {
                int http_code = herr->code;
                format_sts_error(http_code, xml_body, errstr, errstr_size);
                /* STS replaces the generic "failed" with the parsed
                 * AssumeRoleWithWebIdentity-specific code. */
                rd_http_error_destroy(herr);
                herr   = NULL;
                result = (http_code < 0) ? RD_KAFKA_RESP_ERR__TRANSPORT
                                         : RD_KAFKA_RESP_ERR__AUTHENTICATION;
                goto cleanup;
        }

        akid           = rd_aws_xml_extract(xml_body, "AccessKeyId");
        secret         = rd_aws_xml_extract(xml_body, "SecretAccessKey");
        session        = rd_aws_xml_extract(xml_body, "SessionToken");
        expiration_str = rd_aws_xml_extract(xml_body, "Expiration");

        if (!akid || !*akid || !secret || !*secret || !session || !*session) {
                rd_snprintf(errstr, errstr_size,
                            "AssumeRoleWithWebIdentity response missing "
                            "required fields "
                            "(AccessKeyId/SecretAccessKey/SessionToken)");
                result = RD_KAFKA_RESP_ERR__BAD_MSG;
                goto cleanup;
        }

        if (expiration_str && *expiration_str) {
                if (!rd_aws_parse_iso8601_utc(expiration_str, &exp_epoch)) {
                        rd_snprintf(errstr, errstr_size,
                                    "AssumeRoleWithWebIdentity response: "
                                    "malformed <Expiration>: %s",
                                    expiration_str);
                        result = RD_KAFKA_RESP_ERR__BAD_MSG;
                        goto cleanup;
                }
                exp_us = rd_aws_epoch_to_monotonic_us(exp_epoch);
        }

        *credsp = rd_kafka_aws_credentials_new(akid, secret, session, exp_us);
        result  = RD_KAFKA_RESP_ERR_NO_ERROR;

cleanup:
        if (escaped_role_arn)
                curl_free(escaped_role_arn);
        if (escaped_session_name)
                curl_free(escaped_session_name);
        if (escaped_token)
                curl_free(escaped_token);
        RD_IF_FREE(herr, rd_http_error_destroy);
        RD_IF_FREE(headers, curl_slist_free_all);
        RD_IF_FREE(url, rd_free);
        RD_IF_FREE(body, rd_free);
        RD_IF_FREE(xml_body, rd_free);
        RD_IF_FREE(akid, rd_free);
        RD_IF_FREE(secret, rd_free);
        RD_IF_FREE(session, rd_free);
        RD_IF_FREE(expiration_str, rd_free);
        rd_http_req_destroy(&hreq);
        return result;
}
