# DESIGN — `librdkafka-oauthbearer-aws` dlopen plugin

## Status

| Milestone | Scope | Status |
|---|---|---|
| M0 | Scaffolding: stub `conf_init` loads via `plugin.library.paths` | ✅ |
| M1 | aws-sdk-cpp vendored via FetchContent + static build; link verified | ✅ |
| M2 | `sasl.oauthbearer.aws.*` config property registration via interceptor | ✅ |
| M3 | `STSClient::GetWebIdentityTokenCallable` wrapper, real STS wire call verified | ✅ |
| M4 | `oauthbearer_token_refresh_cb` wired end-to-end through rk→ctx registry | ✅ |
| M5 | `Aws::InitAPI` lifecycle documented; never `ShutdownAPI` | ✅ |
| M6 | Symbol hygiene: 3669 → 2 exports; -static-libstdc++ on Linux | ✅ |
| M7 | Static archive `librdkafka-oauthbearer-aws.a` + pkg-config | ✅ |
| M8 | Mocked-STS comprehensive unit tests (`oauthbearer-aws/tests/`) | ✅ |
| M9 | Real-AWS validation harness (env-gated) + EC2 run procedure documented | ✅ code / ⏳ EC2 run |
| M10 | Semaphore CI blocks + aws-sdk-cpp build cache | ⏳ |
| M11 | Packaging (rpm/deb/brew/nuget/static-tar) | ⏳ |
| M12 | CHANGELOG + INTRODUCTION.md/CONFIGURATION.md updates | ⏳ |

## Goal

Add an OAUTHBEARER token-refresh provider that mints JWTs via AWS STS
`GetWebIdentityToken` (IAM Outbound Identity Federation, GA 2025-11-19),
shipped as an **optional plugin** distinct from core librdkafka. Users
who do not opt in see zero change in their link graph, SBOM, or
packaging dependency set.

## Non-goals

- Any change to core librdkafka (`src/`, `src-cpp/`).
- Azure / GCP / Vault OAUTHBEARER — separate plugins if ever needed.
- The native-C approach in `DESIGN_AWS_OAUTHBEARER_V1.md` (a different
  branch of the same AWS integration work). Both approaches stay on the
  table; this document scopes only the plugin approach.
- Packaging for language bindings (confluent-kafka-{go,python,dotnet,js}) —
  they each ship their own AWS integration using the language's native
  AWS SDK. This plugin serves direct C/C++ consumers of librdkafka.

## Why a plugin

Full reasoning lives in the `project_optional_deps_pattern_librdkafka.md`
memory. Two-line summary:

- A direct build-time dep on aws-sdk-cpp (curl/OIDC-style) would drag
  `libstdc++`/`libc++` in as a transitive NEEDED on `librdkafka.so`,
  breaking every downstream language binding that assumes librdkafka
  is pure C. A **dlopen plugin** isolates the AWS SDK and its C++
  runtime behind `plugin.library.paths`; core librdkafka is untouched.
- Cross-language consensus: Go (`kafka/oauthbearer/aws` submodule), .NET
  (`Confluent.Kafka.OAuthBearer.Aws` NuGet), JS (`oauthbearer-aws`
  workspace), Python (`[oauthbearer-aws]` extra) all ship AWS
  integrations as separately-published units. This plugin is the C
  analog.

## Repo layout

All in `oauthbearer-aws/`, sibling to `src/` and `src-cpp/`. Flat
directory, matching the repo convention verified against `src-cpp/`
and `tests/interceptor_test/` (the only other plugin in this repo).

```
oauthbearer-aws/
├── CMakeLists.txt                  # build + pkg-config generation
├── Makefile                        # mklove placeholder (not aws-sdk-cpp-aware)
├── README.md                       # user-facing quickstart
├── rdkafka_oauthbearer_aws.h       # public C header (2 functions)
├── plugin.c                        # conf_init, register(), refresh cb,
│                                   # rk→ctx registry, interceptor wiring
├── config.c / config.h             # parsed config struct + property parser
├── aws_sts_provider.cpp / .h       # C++ wrapper around STSClient
├── exports.list                    # macOS linker exports allowlist
└── rdkafka-oauthbearer-aws.pc.in   # pkg-config template
```

Top-level [CMakeLists.txt](CMakeLists.txt) has
`option(WITH_PLUGIN_OAUTHBEARER_AWS OFF)` + conditional
`add_subdirectory(oauthbearer-aws)` after `src-cpp`. Opt-in by default;
core contributors never build aws-sdk-cpp unless they flip the flag.

## Build

```sh
# Ergonomic local iteration with a pre-existing aws-sdk-cpp checkout:
cmake -B build -DWITH_PLUGIN_OAUTHBEARER_AWS=ON \
      -DAWSSDK_LOCAL_SRC=/path/to/aws-sdk-cpp \
      -DRDKAFKA_BUILD_TESTS=OFF -DRDKAFKA_BUILD_EXAMPLES=OFF
cmake --build build --target rdkafka-oauthbearer-aws -j
cmake --build build --target rdkafka-oauthbearer-aws-static -j
```

CI / release builds omit `-DAWSSDK_LOCAL_SRC` to FetchContent pin
`1.11.694` from GitHub (earliest tag containing `GetWebIdentityToken`;
verified via `git log --follow` on the generated STS model sources).

Produced artifacts:

- **Shared plugin** — `rdkafka-oauthbearer-aws.{so,dylib,dll}` (no `lib`
  prefix, so `plugin.library.paths=rdkafka-oauthbearer-aws` resolves
  directly). Matches the `tests/interceptor_test/` convention.
- **Static archive** — `librdkafka-oauthbearer-aws.a` (with `lib`
  prefix — standard convention for static libs, which are linked not
  dlopen'd). Thin archive: just plugin's own `.o` files. Users link
  it alongside their own librdkafka.a + the aws-sdk-cpp archives
  listed in the generated pkg-config.
- **pkg-config** — `rdkafka-oauthbearer-aws.pc`. Platform-specific:
  macOS adds Apple Security/Network/CoreFoundation frameworks; Linux
  adds `-ls2n -laws-lc -lpthread -ldl -lrt -lm`.

## Runtime architecture

### Lifecycle

```
            User sets plugin.library.paths
                        │
                        ▼
    librdkafka's plugin.c → rd_dl_open()
                        │
                        ▼
     dlsym("conf_init") → invoke → install_plugin()
                        │
          ┌─────────────┼─────────────┐
          ▼             ▼             ▼
  oauthbearer_         register     allocate ctx
  token_refresh_cb     4           
  = oba_refresh_cb     interceptors 
                                      │
            User sets AWS props       │ on_conf_set →
                       ──────────────▶│ rd_kafka_oauthbearer_aws_conf_set
                                      │ (populates ctx fields)
                                      ▼
            rd_kafka_new             on_new →
                       ──────────────▶│ validate, construct STS provider,
                                      │ insert (rk, ctx) in registry
                                      ▼
            SASL handshake            oba_refresh_cb(rk, …) →
            needs token              │ registry.find(rk) → ctx
                                      │ ctx->provider->GetToken() → STS HTTP
                                      │ rd_kafka_oauthbearer_set_token()
                                      ▼
            rd_kafka_destroy         on_conf_destroy →
                       ──────────────▶│ registry.remove(rk)
                                      │ rd_kafka_oauthbearer_aws_conf_destroy
                                      │ (frees provider, cached error, strings)
```

### rk → ctx registry (non-obvious, worth reading the memory)

The refresh-callback ABI passes the **user's conf opaque** as its
third argument, not ours. We cannot reserve that slot. So plugin state
for the callback lives in a static linked-list in [plugin.c](oauthbearer-aws/plugin.c)
keyed by `rd_kafka_t *`, populated at `on_new`, cleaned at
`on_conf_destroy`. Protected by a pthread mutex; linear scan is fine
for small client counts (swap to hash map if profiling warrants).

### Config property pipeline

Per-conf `rd_kafka_oauthbearer_aws_conf_t` ctx allocated at
`conf_init` / `register`. Incrementally populated by `on_conf_set`
interceptor as the user calls `rd_kafka_conf_set()` for each
`sasl.oauthbearer.aws.*` property. Validated at `on_new` (required
fields: audience, region); error recorded on the ctx so the refresh
callback can surface it even though `rd_kafka_new` cannot be blocked
(see below).

### Refresh callback error paths

Four distinct error messages depending on where things went wrong:

1. `"no plugin context registered for this client"` — user ordering bug.
2. `ctx->provider_error` — validation or construction failure at
   `on_new` (captured there, replayed here because librdkafka
   interceptors can't block client creation — see decision #2 below).
3. `"internal error: STS provider not constructed"` — guards against
   future logic errors.
4. Full aws-sdk-cpp error including exception name + message — actual
   STS wire failures.

### STS provider

[aws_sts_provider.cpp](oauthbearer-aws/aws_sts_provider.cpp). Thin
C++ wrapper around `Aws::STS::STSClient::GetWebIdentityTokenCallable`.
Uses aws-sdk-cpp's `DefaultAWSCredentialsProviderChain` (env, IMDSv2,
ECS, EKS IRSA, EKS Pod Identity, SSO, profile). Sync bridge via
`future.get()` on librdkafka's background refresh thread — safe
because there's no `SynchronizationContext`-like deadlock trap.

`Aws::InitAPI` called exactly once per process via `std::call_once`.
`Aws::ShutdownAPI` never called — host app or other plugins may still
be using aws-sdk-cpp. Resources reclaimed at process exit.

## Public API

Two `extern "C"` entry points exported (all other plugin symbols
hidden by [exports.list](oauthbearer-aws/exports.list) on macOS and
`--exclude-libs,ALL` on Linux):

```c
/* dlopen path — invoked by librdkafka's plugin loader when the user
   sets plugin.library.paths=rdkafka-oauthbearer-aws */
rd_kafka_resp_err_t conf_init(rd_kafka_conf_t *conf,
                              void **plug_opaquep,
                              char *errstr, size_t errstr_size);

/* static-link path — explicit call for confluent-kafka-go (cgo),
   distroless containers, embedded deployments */
rd_kafka_resp_err_t rd_kafka_oauthbearer_aws_register(
    rd_kafka_conf_t *conf,
    char *errstr, size_t errstr_size);
```

Both paths install the same refresh callback + four interceptors on
the conf. After `register`, callers set their AWS props via the
standard `rd_kafka_conf_set` and proceed with `sasl.mechanisms=
OAUTHBEARER`, `sasl.oauthbearer.method=default`.

## Config properties

| Property | Required | Default | Validation |
|---|---|---|---|
| `sasl.oauthbearer.aws.audience` | yes | — | non-empty string, passed as `Audience.member.1` |
| `sasl.oauthbearer.aws.region` | yes | — | non-empty string, drives STS regional endpoint |
| `sasl.oauthbearer.aws.signing.algorithm` | | `ES384` | must be `ES384` or `RS256` |
| `sasl.oauthbearer.aws.duration.seconds` | | `300` | integer in `[60, 3600]` |
| `sasl.oauthbearer.aws.sts.endpoint` | | — | must start with `https://`; FIPS/VPC override |

Unknown properties within the `sasl.oauthbearer.aws.*` namespace are
**rejected** (not passed through) — catches typos like `audiance`
instead of silently doing nothing.

## Key design decisions

Each of these has a failure mode that a "simplification" would
re-introduce. Do not undo without reading the rationale.

1. **Plugin binary has no `lib` prefix** (`rdkafka-oauthbearer-aws.so`,
   not `librdkafka-oauthbearer-aws.so`). Matches `tests/interceptor_test/`
   and keeps the `plugin.library.paths` value short. The static archive
   keeps the `lib` prefix because static libs follow the link-line
   convention.

2. **`on_new` errors are warning-only, not fatal**
   ([src/rdkafka_interceptor.c:75-99](src/rdkafka_interceptor.c#L75-L99)).
   Interceptors cannot block `rd_kafka_new()` — it always succeeds.
   Missing required config surfaces as:
   (a) an `ICFAIL` warning at create time (visible to operators),
   (b) a hard auth failure at first token-refresh invocation (where
   we call `rd_kafka_oauthbearer_set_token_failure`). Sibling-language
   clients do the same thing; this is the librdkafka idiom, not a
   workaround.

3. **Refresh-callback `opaque` belongs to the user**, not us. Hence the
   static `rd_kafka_t *` → ctx registry. A plugin that steals the conf
   opaque breaks users who set it via `rd_kafka_conf_set_opaque`. 

4. **`std::call_once` for `Aws::InitAPI`, never `ShutdownAPI`.** No
   refcount needed because we never tear down. Host app or another
   plugin may also be using aws-sdk-cpp; shutting down global SDK
   state at plugin unload would segfault their subsequent calls.
   Cross-language memos reinforce the same decision.

5. **Hidden visibility + platform-specific linker archive-hiding.**
   `CXX_VISIBILITY_PRESET hidden` only affects our own compilation;
   aws-sdk-cpp archives link with default visibility and leak ~3700
   symbols without extra help. Linux: `--exclude-libs,ALL`. macOS:
   explicit [exports.list](oauthbearer-aws/exports.list) via
   `-Wl,-exported_symbols_list`. Final export count: exactly 2.

6. **Plugin statically links its own aws-sdk-cpp + C++ runtime
   (Linux).** The entire point of the plugin pattern is to isolate
   the AWS dependency from core librdkafka. `-static-libstdc++
   -static-libgcc` on Linux means downstream apps using this plugin
   do NOT see libstdc++ as a transitive NEEDED entry. macOS leaves
   libc++ as a dynamic dep because it's OS-provided.

7. **aws-sdk-cpp `-Werror` walker.** Vendored aws-sdk-cpp 1.11.xxx on
   newer compilers (AppleClang 21+) trips its own `-Werror` on upstream
   hygiene drift that AWS hasn't cleaned up. A recursive CMake function
   applies `-Wno-error` to every target inside the FetchContent subtree
   while leaving our own code on `-Werror`. Permanent feature, not a
   temporary hack.

8. **Pure-C entry point, C++ internals**, joined via `extern "C"`
   headers. `plugin.c` and `config.c` are pure C so the plugin ABI is
   C-clean; `aws_sts_provider.cpp` is the only C++ TU and its public
   surface is an opaque handle plus three `extern "C"` functions.

9. **`aws-crt-cpp::ApiHandle` eagerly initialises every CRT subsystem**
   at startup — `aws-c-s3`, `aws-c-mqtt`, etc. — even for a pure-STS
   plugin. Static-link users MUST list those archives in their link
   line (pkg-config template handles this; hand-crafted link commands
   will fail without them). Also `-lz` is always required because
   `RequestCompression` uses zlib unconditionally.

## Running M9 real-AWS validation on EC2

The real-AWS test ([oauthbearer-aws/tests/test_real_aws.c](oauthbearer-aws/tests/test_real_aws.c))
is opt-in via `RD_UT_AWS_PLUGIN=1`; without the gate it prints a skip
message and exits 0. Labelled `real_aws` in CTest so CI can isolate it
(`ctest -L real_aws`).

**Recommended EC2 setup** — match the native-path M7 validation to
enable direct cross-client parity comparison:
- Instance in `eu-north-1` (same region the native path used).
- IAM role `ktrue-iam-sts-test-role` (or equivalent) with
  `sts:GetWebIdentityToken` permitted.
- Account has previously called
  `EnableOutboundWebIdentityFederation` (admin-only; one-time).
- IMDSv2 enabled on the instance — aws-sdk-cpp's
  `DefaultAWSCredentialsProviderChain` picks creds up from there
  automatically when env/profile aren't set.

**Run**:
```sh
cmake -B build -DWITH_PLUGIN_OAUTHBEARER_AWS=ON ...
cmake --build build --target test_oba_real_aws -j
RD_UT_AWS_PLUGIN=1 \
  RD_UT_AWS_AUDIENCE="https://api.example.com" \
  RD_UT_AWS_REGION="eu-north-1" \
  ./build/oauthbearer-aws/tests/test_oba_real_aws
```

**Expected output on success**: JWT length in the ~1200–1500 byte
range (varies by audience string / duration / algorithm), three
`.`-separated segments, expiration 300+ seconds in the future.
Cross-reference `project_aws_outbound_federation.md` M7 (1467 bytes,
native path) and `project_go_aws_oauthbearer.md` / `project_dotnet_aws_oauthbearer.md`
(1256 bytes each) — numbers are only comparable on matched inputs,
so use the same audience / role / region across all clients when
doing parity runs.

**What M9 has NOT yet verified** (needs a human on EC2):
- The actual JWT mint succeeds against real STS.
- Byte-length agrees with sibling clients on matched inputs.
- IMDSv2 credential resolution works end-to-end without env-var overrides.

## Open items — decide before / during M8–M12

1. **aws-sdk-cpp v3 vs v4 line.** v3 (1.11.x currently) is fine; the
   aws-sdk-cpp v2/v3 split doesn't exist the way .NET v3.7/v4 does. No
   action required today; revisit if 2.x ever ships with breaking changes.

2. **Distro package name** — proposed: `librdkafka-oauthbearer-aws`
   (package), `rdkafka-oauthbearer-aws` (plugin binary), `librdkafka-
   oauthbearer-aws.a` (static), matching familiar `libpam-*` /
   `pam_*.so` asymmetry. Needs Confluent release-team sign-off during M11.

3. **NuGet pipeline integration** — the .NET memo flags that existing
   `Confluent.SchemaRegistry.*` packages have no `dotnet pack` line in
   `appveyor.yml` (released through a separate Confluent pipeline).
   The plugin probably follows the same path. Confirm before M11.

4. **CMake `find_package` config** — M7 ships pkg-config only.
   A `rdkafka-oauthbearer-awsConfig.cmake` file is a nice-to-have for
   CMake-based consumers; deferred to M11 packaging polish.

5. **Wire librdkafka-oauthbearer-aws.a into the static-builds tar**
   produced by Confluent's pipeline (the one commit `e0da09cc` added
   s390x to). M11 work.

6. **Default signing algorithm** — `ES384` per cross-client consensus
   (Go/.NET/native-path memos). Settled, documented here, revisit only
   if AWS recommends otherwise.

7. **Principal-name policy** — hardcoded to `"aws-sts-web-identity"`.
   Confluent Cloud uses the JWT `sub` claim; broker-side principal
   doesn't matter for AWS flows. If a use case demands a real
   principal, parse it from the JWT.

## Cross-references

- **Memory**: `project_librdkafka_aws_oauthbearer_plugin.md` (this plugin's implementation state), `project_optional_deps_pattern_librdkafka.md` (architectural pattern), `project_aws_outbound_federation.md` (native-C alternative), `project_{go,dotnet,js,python}_aws_oauthbearer.md` (sibling-language implementations).
- **librdkafka internals worth reading**: [src/rdkafka_plugin.c](src/rdkafka_plugin.c) (plugin loader), [src/rdkafka_interceptor.c](src/rdkafka_interceptor.c) (interceptor dispatch, incl. the warning-only on_new semantics), [src/rddl.c](src/rddl.c) (dlopen abstraction), [tests/0066-plugins.cpp](tests/0066-plugins.cpp) + [tests/interceptor_test/](tests/interceptor_test/) (canonical plugin test template).
- **aws-sdk-cpp static build** — the upstream `STATIC_BUILD.md` plus
  the `BUILD_ONLY="sts"` / `BUILD_SHARED_LIBS=OFF` knob set used in
  [oauthbearer-aws/CMakeLists.txt](oauthbearer-aws/CMakeLists.txt).
