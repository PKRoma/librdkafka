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

/**
 * @brief M3 STS client wrapper.
 *
 * Owns an aws-sdk-cpp STSClient configured for the plugin's audience,
 * region, signing algorithm, duration, and optional endpoint override.
 * Uses the default credential provider chain (env / IMDSv2 / ECS /
 * EKS IRSA / EKS Pod Identity / SSO / profile — the whole chain AWS
 * SDK provides out of the box).
 *
 * Exposes a tiny extern "C" surface so plugin.c (pure C) can construct,
 * invoke, and destroy it without having to know about aws-sdk-cpp types.
 * M4 wires the refresh callback to use this; M5 tightens the Aws::InitAPI
 * lifecycle.
 */

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetWebIdentityTokenRequest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include "aws_sts_provider.h"
#include "config.h"
}

namespace {

/* ------------------------------------------------------------------------- */
/* aws-sdk-cpp lifecycle (M5-finalised rationale — do not "simplify")         */
/* ------------------------------------------------------------------------- */
/*                                                                            */
/* Strategy: InitAPI exactly once per process; NEVER call ShutdownAPI.        */
/*                                                                            */
/* Why std::call_once:                                                        */
/*   - Thread-safe by C++17 standard: simultaneous callers all block until    */
/*     the chosen callable returns; subsequent calls are no-ops.              */
/*   - Exception-safe: if Aws::InitAPI throws, the once_flag is NOT latched,  */
/*     so the next caller retries rather than being silently stuck on a      */
/*     half-initialised SDK.                                                  */
/*   - Cheaper and stricter than an atomic-refcount pattern — we have no use  */
/*     for a refcount because we never tear down.                             */
/*                                                                            */
/* Why never ShutdownAPI:                                                     */
/*   - The host application or another loaded plugin may also be using       */
/*     aws-sdk-cpp. Calling ShutdownAPI at our plugin teardown would free    */
/*     global SDK state out from under them — observed as segfaults in       */
/*     their subsequent SDK calls.                                            */
/*   - aws-sdk-cpp does not expose a reference-counted InitAPI/ShutdownAPI   */
/*     that multiple independent callers could use safely; the AWS-provided  */
/*     guidance for embedding scenarios is "one caller owns the lifecycle".  */
/*     In a plugin we cannot assume ownership, so we only Init and let the   */
/*     process teardown reclaim everything.                                   */
/*   - Cross-language consensus: Go submodule, .NET NuGet, JS workspace,     */
/*     Python extra all avoid an explicit Shutdown for the same reason.      */
/*                                                                            */
/* Known coexistence failure modes (documented, not prevented):              */
/*   - If the host application explicitly calls Aws::ShutdownAPI during      */
/*     the plugin's active lifetime, our next GetToken will fail. There is   */
/*     no SDK-level way to detect or recover from this; it is a host bug.   */
/*   - If the plugin is dlclose'd (librdkafka does not do this today, but   */
/*     a future caller could), the aws-sdk-cpp static archives get unmapped */
/*     while global SDK state still references now-missing code. This is    */
/*     a latent issue for any plugin statically linking a C++ library with  */
/*     process-global state. Not addressed here — revisit if dlclose becomes */
/*     a supported path.                                                     */
/* ------------------------------------------------------------------------- */

std::once_flag g_aws_init_flag;
Aws::SDKOptions g_aws_options;

void ensure_aws_init() {
        std::call_once(g_aws_init_flag,
                       []() { Aws::InitAPI(g_aws_options); });
}

class StsTokenProvider {
       public:
        explicit StsTokenProvider(const rd_kafka_oauthbearer_aws_conf_t *cfg)
            : audience_(cfg->audience ? cfg->audience : ""),
              region_(cfg->region ? cfg->region : ""),
              signing_algorithm_(cfg->signing_algorithm
                                     ? cfg->signing_algorithm
                                     : ""),
              duration_seconds_(cfg->duration_seconds),
              sts_endpoint_(cfg->sts_endpoint ? cfg->sts_endpoint : "") {
                ensure_aws_init();

                Aws::Client::ClientConfiguration client_cfg;
                client_cfg.region = region_.c_str();
                if (!sts_endpoint_.empty()) {
                        client_cfg.endpointOverride = sts_endpoint_.c_str();
                }
                client_ = std::make_unique<Aws::STS::STSClient>(client_cfg);
        }

        int GetToken(char **token_out,
                     int64_t *expiration_ms_out,
                     char *errstr,
                     size_t errstr_size) {
                Aws::STS::Model::GetWebIdentityTokenRequest request;
                request.AddAudience(Aws::String(audience_.c_str()));
                if (!signing_algorithm_.empty()) {
                        request.SetSigningAlgorithm(
                            Aws::String(signing_algorithm_.c_str()));
                }
                if (duration_seconds_ > 0) {
                        request.SetDurationSeconds(duration_seconds_);
                }

                /* GetWebIdentityTokenCallable returns a future-like object.
                 * .get() blocks the current thread until the HTTP call
                 * completes; safe on librdkafka's background refresh thread
                 * since there's no SynchronizationContext equivalent. */
                auto future  = client_->GetWebIdentityTokenCallable(request);
                auto outcome = future.get();

                if (!outcome.IsSuccess()) {
                        const auto &err = outcome.GetError();
                        snprintf(errstr, errstr_size,
                                 "STS GetWebIdentityToken failed: %s: %s",
                                 err.GetExceptionName().c_str(),
                                 err.GetMessage().c_str());
                        return -1;
                }

                const auto &result = outcome.GetResult();
                const auto &jwt    = result.GetWebIdentityToken();
                if (jwt.empty()) {
                        snprintf(errstr, errstr_size,
                                 "STS returned empty WebIdentityToken");
                        return -1;
                }

                char *copy = (char *)std::malloc(jwt.size() + 1);
                if (!copy) {
                        snprintf(errstr, errstr_size,
                                 "out of memory duplicating JWT");
                        return -1;
                }
                std::memcpy(copy, jwt.c_str(), jwt.size() + 1);

                *token_out         = copy;
                *expiration_ms_out = result.GetExpiration().Millis();
                return 0;
        }

       private:
        std::string audience_;
        std::string region_;
        std::string signing_algorithm_;
        int duration_seconds_;
        std::string sts_endpoint_;
        std::unique_ptr<Aws::STS::STSClient> client_;
};

}  // namespace

/* extern "C" layer — the handle is defined here (not in the header) so
 * callers only see an opaque pointer type. */
extern "C" {

struct rd_kafka_aws_sts_provider_s {
        std::unique_ptr<StsTokenProvider> impl;
};

rd_kafka_aws_sts_provider_t *rd_kafka_aws_sts_provider_new(
    const rd_kafka_oauthbearer_aws_conf_t *conf,
    char *errstr,
    size_t errstr_size) {
        if (!conf || !conf->audience || !conf->region) {
                snprintf(errstr, errstr_size,
                         "provider requires audience and region");
                return nullptr;
        }
        try {
                auto holder = std::make_unique<rd_kafka_aws_sts_provider_s>();
                holder->impl = std::make_unique<StsTokenProvider>(conf);
                return holder.release();
        } catch (const std::exception &e) {
                snprintf(errstr, errstr_size,
                         "provider construction failed: %s", e.what());
                return nullptr;
        } catch (...) {
                snprintf(errstr, errstr_size,
                         "provider construction failed: unknown exception");
                return nullptr;
        }
}

void rd_kafka_aws_sts_provider_destroy(rd_kafka_aws_sts_provider_t *p) {
        if (!p)
                return;
        delete p;
}

int rd_kafka_aws_sts_provider_get_token(rd_kafka_aws_sts_provider_t *p,
                                         char **token_out,
                                         int64_t *expiration_ms_out,
                                         char *errstr,
                                         size_t errstr_size) {
        if (!p || !token_out || !expiration_ms_out) {
                snprintf(errstr, errstr_size, "invalid arguments");
                return -1;
        }
        try {
                return p->impl->GetToken(token_out, expiration_ms_out, errstr,
                                         errstr_size);
        } catch (const std::exception &e) {
                snprintf(errstr, errstr_size,
                         "STS call threw exception: %s", e.what());
                return -1;
        } catch (...) {
                snprintf(errstr, errstr_size,
                         "STS call threw unknown exception");
                return -1;
        }
}

}  // extern "C"
