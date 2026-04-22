# librdkafka-oauthbearer-aws

AWS OAUTHBEARER token provider plugin for librdkafka. Mints JWTs via
AWS STS `GetWebIdentityToken` (AWS IAM Outbound Identity Federation)
and hands them to librdkafka through the standard OAUTHBEARER refresh
callback. The AWS SDK and all its transitive dependencies are isolated
to this plugin; consumers of librdkafka who do not set
`plugin.library.paths` see zero change in their link graph.

## Status

**Functional (M0–M7 complete).** Plugin is end-to-end working against
real AWS STS; remaining milestones (M8 unit tests with mocked STS,
M9 real-AWS validation on EC2, M10 Semaphore CI, M11 packaging,
M12 CHANGELOG + docs) are hardening and distribution. See
[../DESIGN_PLUGIN_OAUTHBEARER_AWS.md](../DESIGN_PLUGIN_OAUTHBEARER_AWS.md)
at the repo root for the full design, milestone table, and rationale
behind non-obvious decisions.

## Build

Opt in via `-DWITH_PLUGIN_OAUTHBEARER_AWS=ON`:

```sh
cmake -B build -DWITH_PLUGIN_OAUTHBEARER_AWS=ON
cmake --build build -j
```

The resulting artifact is `oauthbearer-aws/rdkafka-oauthbearer-aws.so`
(or `.dylib` / `.dll`), ready for `dlopen`.

## Usage

### dlopen path (shared plugin)

```
plugin.library.paths=rdkafka-oauthbearer-aws
sasl.mechanisms=OAUTHBEARER
sasl.oauthbearer.method=default
sasl.oauthbearer.aws.audience=https://kafka.example.com
sasl.oauthbearer.aws.region=eu-north-1
sasl.oauthbearer.aws.duration.seconds=600
```

### Static-link path (`librdkafka-static.a` consumers)

Call `rd_kafka_oauthbearer_aws_register()` from application startup
and link `librdkafka-oauthbearer-aws.a` + the transitive aws-sdk-cpp
archives (see `pkg-config --libs --static rdkafka-oauthbearer-aws`).
Intended for confluent-kafka-go (cgo), distroless/Alpine-static
containers, embedded deployments.

## aws-sdk-cpp coexistence

The plugin statically links its own copy of aws-sdk-cpp (STS + Core + CRT)
with hidden visibility (M6), so its SDK instance is independent of any
aws-sdk-cpp the host application may be using for other purposes.

Global aws-sdk-cpp state (allocators, logging, HTTP client registry) is
initialised **once per process** via `std::call_once` the first time a
client is created with the plugin registered. The plugin deliberately
never calls `Aws::ShutdownAPI` — tearing down SDK state at plugin
unload would break any host code that is also using aws-sdk-cpp.
Resources are reclaimed when the process exits.

One known caveat: if the host application explicitly calls
`Aws::ShutdownAPI` during the plugin's active lifetime, the next token
refresh will fail. This is a host-application bug; the plugin cannot
detect or recover from it.
