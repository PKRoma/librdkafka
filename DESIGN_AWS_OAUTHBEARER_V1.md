# AWS OAUTHBEARER via `sts:GetWebIdentityToken` — V1 Implementation Plan

**Status:** Planning · **Scope:** V1 — credential provider chain + STS `GetWebIdentityToken` client, **no** SASL wiring yet
**Target platforms covered by V1:** EC2, EKS (IRSA + Pod Identity), ECS, Fargate, Lambda

---

## 1. Goals and non-goals

### Goals (V1)

1. Resolve AWS credentials from the four provider sources that cover all 5 target platforms: `env`, `web_identity`, `ecs`, `imds` — in a chain with C++ SDK default order.
2. Call `sts:GetWebIdentityToken` on a regional STS endpoint, signed with those credentials via libcurl's `CURLOPT_AWS_SIGV4`, parse the response, surface the JWT + expiry.
3. Build a small, self-contained test that exercises the chain + STS call end-to-end against local mock endpoints, proving the architecture works before any SASL integration.
4. Zero new build-time dependencies. Stay within the existing libcurl + OpenSSL + cJSON + base64 toolkit already in librdkafka.

### Non-goals (explicitly deferred)

- **No SASL wiring.** [src/rdkafka_sasl_oauthbearer.c](src/rdkafka_sasl_oauthbearer.c) and [src/rdkafka_conf.c](src/rdkafka_conf.c) are untouched in V1.
- **No new config properties** (`sasl.aws.*`). All inputs go through function arguments and environment variables.
- **No shared credentials file** (`~/.aws/credentials` / `~/.aws/config`). Deferred to V2.
- **No `credential_process`, SSO, Cognito, Login, X509, Delegate providers.** Not relevant to the 5 target platforms.
- **No auto-refresh timer integration with `rd_kafka_timers_t`.** V1's cache refreshes on-demand (lazy). Proactive background refresh is V2.
- **No profile-based role chaining (`source_profile` + `role_arn`).** V2 if demanded.

---

## 2. Prerequisite probes (do before writing any C code)

**Status (2026-04-21): ALL PROBES PASSED** on an EC2 instance in `eu-north-1` with curl 8.17.0 / libcurl 8.17.0 / OpenSSL 3.5.5. Design proceeds unchanged with three small additions folded into the relevant milestones:
- **ISO 8601 parser** must support fractional seconds with explicit offset (e.g. `2026-04-21T06:06:47.641000+00:00`) AND `...Z`-suffix without fractions. Both appear in STS responses. (Affects M3.4, M5.1, M7.5.)
- **Web identity token file** must have trailing whitespace stripped before being put into the POST body. (M5.2.)
- **Region resolution** must be explicit in librdkafka — fail loud if unresolvable. Do NOT silently default to `us-east-1` or IMDS-sniff. (M7.1.)

No hand-signing code needed — libcurl's `CURLOPT_AWS_SIGV4` handles session tokens correctly via `CURLOPT_HTTPHEADER`.

---

The two open questions that gated implementation decisions. Both settled with shell commands before committing to structure. Expected effort: ~1–2 hours.

### Probe A — `CURLOPT_AWS_SIGV4` with session token

**Question:** Does libcurl include `X-Amz-Security-Token` in the SigV4 `SignedHeaders` list when added via `CURLOPT_HTTPHEADER`?

**Procedure:** Obtain temporary creds (e.g. from an IAM role via `aws sts assume-role`) and run:

```bash
curl -v \
  --aws-sigv4 "aws:amz:us-east-1:sts" \
  --user "${AKID}:${SECRET}" \
  -H "X-Amz-Security-Token: ${SESSION_TOKEN}" \
  -d "Action=GetCallerIdentity&Version=2011-06-15" \
  https://sts.us-east-1.amazonaws.com/
```

**Pass criteria:** HTTP 200 and `<GetCallerIdentityResponse>` in body. Inspect the `Authorization` header in `-v` output — `SignedHeaders=` list must include `x-amz-security-token`.

**If it fails:** We add a small extension — sign by hand using OpenSSL HMAC-SHA256 and the published SigV4 key-derivation chain (~200 LoC added on top of chosen approach). Design remains otherwise unchanged.

### Probe B — `GetWebIdentityToken` wire format

**Question:** Does the response come back as XML (classic STS) or JSON, and what's the exact request shape?

**Procedure:** Run against a real AWS account with outbound federation enabled (one-time `aws iam enable-outbound-web-identity-federation` if not yet done):

```bash
aws sts get-web-identity-token \
  --audience "https://example.com" \
  --signing-algorithm ES384 \
  --duration-seconds 300 \
  --region us-east-1 \
  --debug 2>&1 \
  | grep -E 'Request URL|Request method|Request headers|Response body|Content-Type' \
  | head -40
```

**Capture:** exact request URL, method, `Content-Type` of body, actual body bytes, response `Content-Type`, response body. Write these into `DESIGN_AWS_OAUTHBEARER_V1_PROBE.md` alongside this doc for reference during implementation.

**Decision output:**
- If response is JSON → reuse cJSON path from [src/rdhttp.c](src/rdhttp.c).
- If response is XML → write a ~30-line hand parser for two fields (`WebIdentityToken`, `Expiration`). Do **not** pull libxml2.

### Probe C — `AssumeRoleWithWebIdentity` format (same question for EKS IRSA path)

Same procedure as Probe B but for the existing, classic STS action:

```bash
aws sts assume-role-with-web-identity \
  --role-arn "arn:aws:iam::123:role/test" \
  --role-session-name "test" \
  --web-identity-token "$(cat ${AWS_WEB_IDENTITY_TOKEN_FILE})" \
  --region us-east-1 \
  --debug 2>&1 \
  | grep -E 'Request URL|Request method|Request headers|Response body|Content-Type'
```

Classic STS is known-XML. Confirm, then scope the mini XML parser once and use it for both STS responses.

---

## 3. File layout

Two new files in `src/`, plus one test program.

### New source files

| File | Purpose | Est. LoC |
|---|---|---|
| `src/rdkafka_aws_credentials.h` | Public (internal) types: `rd_kafka_aws_credentials_t`, `rd_kafka_aws_creds_provider_t`. Provider factory prototypes. | ~120 |
| `src/rdkafka_aws_credentials.c` | Env / IMDS / ECS / web_identity providers, chain dispatcher, cached wrapper, credential struct refcount/destroy. | ~650–800 |
| `src/rdkafka_aws_sts.h` | `rd_kafka_aws_sts_get_web_identity_token()`, `rd_kafka_aws_sts_assume_role_with_web_identity()`. Response types. | ~60 |
| `src/rdkafka_aws_sts.c` | STS client. SigV4 via `CURLOPT_AWS_SIGV4`. Request body builder, response parser (JSON or XML per probe). | ~300–400 |

### New test file (V1 milestone 6 only)

| File | Purpose | Est. LoC |
|---|---|---|
| `tests/aws_v1_standalone_test.c` | **Not** registered in `test.c` / `CMakeLists.txt`. Standalone binary run via `make aws_v1_test`. Spins up in-process mock HTTP servers on loopback ports, drives the full chain + STS path, asserts results. | ~400–500 |

### Unit tests inside `rdunittest.c`

New `unittest_aws_credentials()` entry added to the registry in `src/rdunittest.c`. Pure logic — no network. Exercises env parsing, chain dispatch with fake providers, credential struct refcount/destroy, expiry arithmetic.

---

## 4. Data structures and interface contracts

### 4.1 Credentials type

```c
/* src/rdkafka_aws_credentials.h */

typedef struct rd_kafka_aws_credentials_s {
        char *access_key_id;          /* required */
        char *secret_access_key;      /* required */
        char *session_token;          /* NULL for long-lived creds */
        rd_ts_t expiration_us;        /* rd_clock()-basis monotonic; 0 = never */
        rd_refcnt_t refcnt;
} rd_kafka_aws_credentials_t;

rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_new(const char *akid,
                             const char *secret,
                             const char *session_token,
                             rd_ts_t expiration_us);

rd_kafka_aws_credentials_t *
rd_kafka_aws_credentials_keep(rd_kafka_aws_credentials_t *c);

void rd_kafka_aws_credentials_destroy(rd_kafka_aws_credentials_t *c);

rd_bool_t rd_kafka_aws_credentials_expired(
    const rd_kafka_aws_credentials_t *c, rd_ts_t now_us);
```

**Contract:** credentials are immutable once constructed. `_keep` increments refcount; `_destroy` decrements and frees at zero. Safe to share across threads.

### 4.2 Provider interface

```c
typedef struct rd_kafka_aws_creds_provider_s rd_kafka_aws_creds_provider_t;

typedef enum {
        RD_KAFKA_AWS_CREDS_OK       = 0,
        RD_KAFKA_AWS_CREDS_SKIP     = 1, /* provider not applicable, try next */
        RD_KAFKA_AWS_CREDS_FATAL    = 2, /* applicable but failed; surface to caller */
} rd_kafka_aws_creds_result_t;

typedef rd_kafka_aws_creds_result_t (*rd_kafka_aws_creds_resolve_fn)(
    rd_kafka_aws_creds_provider_t *self,
    rd_kafka_aws_credentials_t **credsp,
    char *errstr, size_t errstr_size);

struct rd_kafka_aws_creds_provider_s {
        const char *name;                        /* "env", "imds", ... */
        rd_kafka_aws_creds_resolve_fn resolve;
        void (*destroy)(rd_kafka_aws_creds_provider_t *);
        rd_kafka_t *rk;                          /* for logging; may be NULL in UT */
        void *opaque;                            /* provider-specific */
};
```

**Contract:** the `SKIP` vs `FATAL` distinction is load-bearing. Missing-env-var is `SKIP` (chain tries next). IMDS returning 403 is `FATAL` (identity exists but is misconfigured — don't silently fall back to something weaker).

### 4.3 Provider factories

```c
/* env: AWS_ACCESS_KEY_ID / _SECRET_ACCESS_KEY / _SESSION_TOKEN */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_env_new(rd_kafka_t *rk);

/* IMDSv2 on 169.254.169.254, honours AWS_EC2_METADATA_DISABLED and
 * AWS_EC2_METADATA_SERVICE_ENDPOINT (for test redirect). */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_imds_new(rd_kafka_t *rk);

/* Either RELATIVE_URI mode (ECS on 169.254.170.2) or FULL_URI mode
 * (EKS Pod Identity on 169.254.170.23). Optional bearer token via
 * AWS_CONTAINER_AUTHORIZATION_TOKEN or ..._FILE (re-read each fetch). */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_ecs_new(rd_kafka_t *rk);

/* AWS_WEB_IDENTITY_TOKEN_FILE + AWS_ROLE_ARN -> AssumeRoleWithWebIdentity.
 * Unauthenticated STS call (no SigV4). */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_web_identity_new(rd_kafka_t *rk,
                                             const char *region);

/* Walks providers in order. Returns first OK. Stops on FATAL. */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_chain_new(rd_kafka_t *rk,
                                      rd_kafka_aws_creds_provider_t **providers,
                                      size_t n_providers);

/* Convenience: env -> web_identity -> ecs -> imds */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_default_chain_new(rd_kafka_t *rk,
                                              const char *region);

/* Wraps any provider with an expiry-driven cache. */
rd_kafka_aws_creds_provider_t *
rd_kafka_aws_creds_provider_cached_new(rd_kafka_aws_creds_provider_t *inner);
```

**Ownership:** `_chain_new` and `_cached_new` take ownership of passed-in providers; destroying the wrapper destroys the wrapped.

### 4.4 STS client

```c
/* src/rdkafka_aws_sts.h */

typedef struct rd_kafka_aws_sts_jwt_s {
        char *token;            /* the JWT */
        rd_ts_t expiration_us;
} rd_kafka_aws_sts_jwt_t;

void rd_kafka_aws_sts_jwt_destroy(rd_kafka_aws_sts_jwt_t *j);

rd_kafka_resp_err_t
rd_kafka_aws_sts_get_web_identity_token(
    rd_kafka_t *rk,
    const char *region,
    const char *audience,
    const char *signing_algorithm,   /* "ES384" | "RS256" */
    int duration_seconds,
    const rd_kafka_aws_credentials_t *creds,
    rd_kafka_aws_sts_jwt_t **jwtp,
    char *errstr, size_t errstr_size);

rd_kafka_resp_err_t
rd_kafka_aws_sts_assume_role_with_web_identity(
    rd_kafka_t *rk,
    const char *region,
    const char *role_arn,
    const char *role_session_name,
    const char *web_identity_token,
    rd_kafka_aws_credentials_t **credsp,
    char *errstr, size_t errstr_size);
```

**Endpoint override:** both functions consult `AWS_STS_ENDPOINT_URL` env var for test/VPC redirect, falling back to `https://sts.<region>.amazonaws.com/`.

---

## 5. Implementation phases

Ordered by dependencies. Each phase has a concrete "definition of done" and test.

### Milestone 1 — Proof of concept: env provider + chain (the "small test" that bolsters the approach) ✅ DONE

**Scope:** In-memory only. No HTTP. No STS. Just types, env, chain.

**Tasks:**
- M1.1 Credentials struct + refcount lifecycle.
- M1.2 Env provider (`getenv` the three vars; `SKIP` if `AWS_ACCESS_KEY_ID` is absent).
- M1.3 Chain dispatcher with `OK`/`SKIP`/`FATAL` semantics.
- M1.4 `unittest_aws_credentials()` added to `rdunittest.c`. Covers:
  - Credentials `_keep`/`_destroy` refcount correctness.
  - Env provider: all three vars set → OK; only AKID+SECRET → OK with NULL session token; no AKID → SKIP; AKID set but SECRET missing → FATAL.
  - Chain with two fake providers: first SKIPs, second OKs → chain returns second's creds. First FATALs → chain FATALs. All SKIP → chain SKIP.
  - Expiry arithmetic: `expired()` returns true past `expiration_us`, false before.

**Definition of done:**
- `TESTS=0000 RD_UT_TEST=aws_credentials make` passes.
- No network, no leaks under ASAN.
- Code reviewed for style (`make style-check-changed`).

**LoE:** ~150 LoC implementation + ~200 LoC unit tests. **~1 day of focused work.**

**This is the "small test that bolsters the approach"** — after this milestone, the interface design, memory model, and chain semantics are validated. Everything else is adding providers behind the same interface.

---

### Milestone 2 — HTTP helpers for AWS patterns ✅ DONE

**Scope:** Small extensions to `rdhttp` for patterns AWS needs that the OIDC path didn't.

**Tasks:**
- M2.1 ✅ `rd_http_put()` added to [src/rdhttp.h](src/rdhttp.h) and [src/rdhttp.c](src/rdhttp.c). Supports custom headers, optional request body, retry+backoff mirroring `rd_http_get()`. Sends `Content-Length: 0` for zero-body PUTs (IMDSv2-compatible). ~100 LoC.
- M2.2 ✅ No new code needed — existing `rd_http_get()` already accepts `char **headers_array, size_t headers_array_cnt`. Same parameter shape used by `rd_http_get_json()`. Noted in the M3 implementation plan below.

**Definition of done:** ✅ Both helpers callable; build clean; `unittest_http` still passes; new `unittest_http_put` added (gated on `RD_UT_IMDS=1` env var, runs against the real 169.254.169.254 endpoint when set, skips otherwise with a clear message). Full mock-server coverage for PUT arrives in M3.

**Actual LoE:** ~100 LoC added. **~0.5 day as estimated.**

---

### Milestone 3 — IMDSv2 provider ✅ DONE

**Scope:** Single provider, fully testable against a local mock.

**Tasks:**
- M3.1 PUT session token (5-min TTL), capture response body as token.
- M3.2 GET `/latest/meta-data/iam/security-credentials/` → role name (text response).
- M3.3 GET `/latest/meta-data/iam/security-credentials/<role>` → JSON creds.
- M3.4 Parse JSON: `AccessKeyId`, `SecretAccessKey`, `Token`, `Expiration` (ISO8601 — add a small parser or reuse what's in librdkafka).
- M3.5 Honour `AWS_EC2_METADATA_DISABLED=true` → immediate SKIP.
- M3.6 Honour `AWS_EC2_METADATA_SERVICE_ENDPOINT` for mock redirect.
- M3.7 Retry policy: 3 retries with 100/200/400 ms backoff for transient 5xx and connect errors. No retry on 403/404 (role not attached → FATAL).

**Test:** In-process mock HTTP server in a separate thread, binds to `127.0.0.1:<ephemeral>`, serves the three IMDS endpoints. `AWS_EC2_METADATA_SERVICE_ENDPOINT=http://127.0.0.1:<port>` redirects the provider. Assert fetched creds match mock's response.

**Definition of done:**
- Provider fetches correct creds from mock.
- `AWS_EC2_METADATA_DISABLED=true` short-circuits to SKIP without hitting the network.
- Retry behaviour observable in logs.
- No leaks under ASAN including on the failure path.

**LoE:** ~180 LoC provider + ~200 LoC mock server + ~100 LoC test. **~2 days.**

**Actual outcome:** 5 new IMDS tests (happy path, disabled, unreachable, no-role, malformed JSON), all passing. `ut_iso8601` added for the parser itself. Mock HTTP server is `ifndef _WIN32` — Windows port deferred. `rk_terminate` / SSL-conf fields are accessed by rdhttp even for plain-HTTP URLs, so the tests allocate a zero-initialised `rd_kafka_t` via `rd_calloc()` rather than passing NULL. A portable `rd_aws_ymd_hms_to_epoch()` using Howard Hinnant's civil-from-days formula replaces the non-portable `timegm()` / `_mkgmtime()` pair, keeping the date parser platform-independent.

---

### Milestone 4 — ECS provider ✅ DONE (implementation + mock tests; real ECS/Fargate/Pod-Identity validation is a future bring-up)

**Scope:** The two-mode ECS container provider.

**Tasks:**
- M4.1 Detect mode: `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` → prepend `http://169.254.170.2`. `AWS_CONTAINER_CREDENTIALS_FULL_URI` → use as-is, validate against allowlist (loopback, 169.254.170.0/24, 169.254.170.23, `[fd00:ec2::23]`).
- M4.2 Bearer token resolution: prefer `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE` (re-read each fetch — Pod Identity rotates it). Fall back to `AWS_CONTAINER_AUTHORIZATION_TOKEN` (static).
- M4.3 GET the endpoint, parse JSON: `AccessKeyId`, `SecretAccessKey`, `Token`, `Expiration`.
- M4.4 Neither env var set → SKIP.
- M4.5 Full URI outside allowlist → FATAL with a clear error (this is a security boundary).

**Test:** Same mock-server harness as M3. Two sub-tests: RELATIVE_URI mode and FULL_URI + token-file mode. Third sub-test: disallowed host returns FATAL before any HTTP.

**LoE:** ~130 LoC provider + ~80 LoC test. **~1–1.5 days.**

---

### Milestone 5 — web_identity provider + `AssumeRoleWithWebIdentity` ✅ DONE (implementation + mock tests; real EKS IRSA validation requires an actual EKS deployment)

**Scope:** File-sourced JWT → unauthenticated STS call → temp creds. Biggest single milestone.

**Tasks:**
- M5.1 Implement `rd_kafka_aws_sts_assume_role_with_web_identity()` in `rdkafka_aws_sts.c`. Unauthenticated POST (no `Authorization` header — confirmed via Probe C), `Content-Type: application/x-www-form-urlencoded`, XML response (same parser as GetWebIdentityToken).
- M5.2 Provider: reads `AWS_WEB_IDENTITY_TOKEN_FILE` content fresh each call, combines with `AWS_ROLE_ARN`, optional `AWS_ROLE_SESSION_NAME` (generate one if not set). **Strip trailing whitespace/newlines from the token file** — Kubernetes projected tokens and `echo`-written test fixtures both end with `\n`, and the CLI passes that through in the request body (confirmed via Probe C). The API tolerates it, but it's a real-world gotcha to avoid.
- M5.3 Missing env vars or unreadable token file → SKIP.
- M5.4 STS error (InvalidIdentityToken, ExpiredToken, AccessDenied) → FATAL with error code surfaced.

**Test:** Mock STS endpoint serves fixed AssumeRoleWithWebIdentity response. Write a JWT string to a tmp file. Set env vars. Assert provider returns expected creds.

**LoE:** ~200 LoC provider + ~150 LoC STS function + ~150 LoC test. **~3 days.**

---

### Milestone 6 — Cached wrapper

**Scope:** Lazy-refresh cache layer.

**Tasks:**
- M6.1 Cache holds current `rd_kafka_aws_credentials_t *`, a mutex, and the inner provider.
- M6.2 `resolve()` returns cached creds if present AND not within 5 min of expiry. Otherwise calls inner provider, swaps in new creds, returns.
- M6.3 Concurrency model: readers take a ref under the mutex; refresh happens under the mutex with an in-flight flag so only one refresh races. Document the invariant.
- M6.4 On inner FATAL with cached creds still valid → return cached (log warning). On inner FATAL with no cache or expired cache → propagate FATAL.

**Test:** Unit test with a fake provider that increments a counter. Assert: first call hits inner, second call (before expiry) does not, call after expiry does. Multi-threaded stress test: N threads resolving concurrently on a cache with 10 ms expiry; count inner invocations, verify no crashes, no leaks.

**LoE:** ~150 LoC cached + ~150 LoC tests. **~2 days.**

---

### Milestone 7 — `GetWebIdentityToken` STS client ✅ DONE

**Validated end-to-end against real AWS STS on EC2 Amazon Linux 2023** (2026-04-21). The `ut_sts_real` test performed: IMDSv2 credential fetch → `CURLOPT_AWS_SIGV4`-signed POST to `sts.eu-north-1.amazonaws.com/` → HTTP 200 with 1467-byte AWS-signed JWT → parsed Expiration matching the requested 300 s duration (299 s observed including round-trip). This proves libcurl's SigV4 signing is correct when driven from our C code, and that the full Probe A/B success path works autonomously.

Note on test fixture: the IAM role used (`ktrue-iam-sts-test-role`) has an `sts:IdentityTokenAudience` condition restricting the allowed audience to `https://api.example.com` — `ut_sts_real` defaults to that value and can be overridden via the `RD_UT_AWS_AUDIENCE` env var for different policies. An audience mismatch surfaces as `AccessDenied` "no identity-based policy allows" at AWS, which is a policy-level rejection (our code is fine).

**Scope:** The main output — call the new 2025 API and get a JWT.

**Tasks:**
- M7.1 Build regional endpoint URL: `AWS_STS_ENDPOINT_URL` override, else `https://sts.<region>.amazonaws.com/`.
- M7.2 Build form body: `Action=GetWebIdentityToken&Version=2011-06-15&Audience.member.1=<aud>&SigningAlgorithm=<alg>&DurationSeconds=<n>`.
- M7.3 libcurl setup: `CURLOPT_AWS_SIGV4="aws:amz:<region>:sts"`, `CURLOPT_USERPWD=<akid>:<secret>`, `CURLOPT_POSTFIELDS=<body>`, `CURLOPT_HTTPHEADER` with `X-Amz-Security-Token` if session token present.
- M7.4 Compile-time guard: `#if LIBCURL_VERSION_NUM < 0x074b00` → emit `#error` or stub that returns `RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED`. Runtime version check via `curl_version_info()` that returns a clear error if the runtime libcurl is too old even though the build-time one wasn't.
- M7.5 Response parsing (XML or JSON per Probe B): extract `WebIdentityToken`, parse ISO8601 `Expiration`.
- M7.6 Error response parsing: extract AWS error code/message, surface in `errstr`.

**Test:** Mock STS endpoint serves fixed `GetWebIdentityToken` response. Pass static creds. Assert returned JWT matches mock's fixture. One failure path: mock returns 400 with `<Error><Code>InvalidAudience</Code>...`, assert `errstr` contains `InvalidAudience`.

**LoE:** ~250 LoC STS function + ~150 LoC test. **~2–3 days.**

---

### Milestone 8 — End-to-end integration test

**Scope:** The "small test to bolster the approach" in its full form. **This is what actually proves V1 works.**

**Tasks:**
- M8.1 Standalone binary `tests/aws_v1_standalone_test.c`. Builds via a new `aws_v1_test` target in `tests/Makefile`. Links against `librdkafka.a`.
- M8.2 Spins up 3 mock HTTP servers on ephemeral ports: IMDS, ECS, STS. Uses the mock-server code extracted from M3.
- M8.3 Drives four scenarios end-to-end:
  1. **EC2 path:** Set `AWS_EC2_METADATA_SERVICE_ENDPOINT` to IMDS mock. Resolve chain → assert creds come from IMDS mock. Call `GetWebIdentityToken` → assert JWT from STS mock.
  2. **ECS path:** Set `AWS_CONTAINER_CREDENTIALS_FULL_URI` to ECS mock. Resolve chain → assert creds from ECS mock. Call STS → assert JWT.
  3. **EKS IRSA path:** Write token file, set `AWS_WEB_IDENTITY_TOKEN_FILE` + `AWS_ROLE_ARN`, redirect STS to mock. Resolve chain → STS call happens twice (AssumeRoleWithWebIdentity then GetWebIdentityToken).
  4. **Lambda path:** Set env creds directly. Resolve chain → env wins. Call STS → assert JWT.
- M8.4 Run under ASAN (`./dev-conf.sh asan` + rebuild). Must report no leaks.

**Definition of done:**
- `make aws_v1_test && ./aws_v1_test` passes all four scenarios.
- ASAN clean.
- Total runtime < 5 seconds.

**LoE:** ~400 LoC test + ~100 LoC mock-server harness (mostly reused from M3). **~2 days.**

---

## 6. Build integration

### `src/Makefile`
Add new `.c` files to `SRCS`:
```
SRCS_$(WITH_CURL) += rdkafka_aws_credentials.c rdkafka_aws_sts.c
```
Gate on `WITH_CURL` — without libcurl the feature is not buildable.

### `src/CMakeLists.txt`
Mirror under the `WITH_CURL` conditional.

### `win32/librdkafka.vcxproj`
Add entries under the same WITH_CURL conditional group as `rdhttp.c`.

### `tests/Makefile`
New target:
```make
aws_v1_test: $(BIN) aws_v1_standalone_test.c
	$(CC) $(CFLAGS) -I../src aws_v1_standalone_test.c \
	    -L../src -lrdkafka -o $@
```
Not added to `all:` — intentionally separate.

### mklove
No changes. `CURLOPT_AWS_SIGV4` detection is runtime-only for V1 (since no hard build-time dep yet).

---

## 7. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Probe A fails (session token not signed) | Low | Medium | Hand-sign with OpenSSL HMAC — ~200 LoC addition, design otherwise unchanged |
| Probe B returns XML (likely, classic STS) | Medium | Low | Write ~30-line mini-parser for two fields. Hard cap at "do not pull libxml2". |
| IMDSv2 behaviour differs on container hop-limit=1 | Medium | Low | Document limitation; error surfaces via M3.7 retry path |
| ECS full-URI allowlist rejects a legitimate endpoint we didn't know about | Low | Medium | Config escape hatch in V2 (`sasl.aws.ecs.allow_any_endpoint=true`) — not V1 |
| Clock skew with STS (±5min window) | Medium | Medium | Surface as explicit error; document `Expiration` parsing as UTC. M7.6 must not swallow clock-skew errors |
| Mock HTTP server flaky in CI under load | Medium | Low | Ephemeral port + generous connect timeouts; no shared fixtures |
| Concurrent cache refresh race | Medium | Medium | M6.3 invariant explicit. Stress test in M6 is load-bearing. Run under TSAN (`./dev-conf.sh tsan`) before merge |
| `AWS_EC2_METADATA_SERVICE_ENDPOINT` quirks with scheme/trailing-slash | Low | Low | Normalise once at provider construction |

---

## 8. What V1 does not answer (V2 backlog)

These are explicitly out of scope. Revisit after V1 ships and is wired into SASL.

- `~/.aws/credentials` + `~/.aws/config` INI parsing, profile selection, `source_profile` chaining.
- `credential_process` provider.
- SSO / Cognito / Login / X509.
- Proactive background refresh scheduled via `rd_kafka_timers_t` (V1 is lazy-on-demand).
- IPv6 IMDS endpoint (`fd00:ec2::254`) via `AWS_EC2_METADATA_SERVICE_ENDPOINT_MODE`.
- FIPS / dualstack STS endpoints via config property (V1 requires env-based override).

---

## 9. Milestone timeline summary

| Milestone | LoE | Days | Cumulative |
|---|---|---|---|
| 0 — Probes | 0 | 0.25 | 0.25 |
| 1 — Env + chain + PoC test | 350 | 1 | 1.25 |
| 2 — HTTP helpers | 50 | 0.5 | 1.75 |
| 3 — IMDSv2 | 480 | 2 | 3.75 |
| 4 — ECS | 210 | 1.25 | 5 |
| 5 — web_identity | 500 | 3 | 8 |
| 6 — Cached wrapper | 300 | 2 | 10 |
| 7 — GetWebIdentityToken | 400 | 2.5 | 12.5 |
| 8 — E2E standalone test | 500 | 2 | 14.5 |
| **V1 total** | **~2,790** | **~14–15 days** | |

One engineer, focused. Overhead (review, style fixes, edge-case bugs) adds ~20–30%. Realistic elapsed time: **3–4 weeks**.

After V1 lands as a reviewable standalone unit, SASL wiring is a small follow-up PR (~200–300 LoC of glue + config properties + integration test).

---

## 10. Acceptance criteria for V1

The V1 PR is mergeable when **all** of the following hold:

1. All eight milestones complete with their DoD met.
2. `make aws_v1_test` passes all four scenarios end-to-end.
3. ASAN and TSAN report clean on the standalone test.
4. `make style-check-changed` clean.
5. `TESTS=0000 RD_UT_TEST=aws_credentials make` passes.
6. No changes to `rdkafka_sasl_oauthbearer.c` or `rdkafka_conf.c`.
7. No new public API symbols (all new headers are `src/rdkafka_aws_*.h`, internal).
8. No new build-time dependencies.
9. CHANGELOG entry drafted (as "added in upcoming release, not user-visible yet").
10. Probe outputs from phase 0 committed as `DESIGN_AWS_OAUTHBEARER_V1_PROBE.md`.

---

## 11. The "small test to bolster the approach" — immediate next action

If we want the fastest signal that the approach works, **Milestone 1 alone** is that signal. It validates:

- Interface shape (`rd_kafka_aws_creds_provider_t` + resolve-fn pattern).
- Memory model (refcounted credentials, provider ownership).
- Chain semantics (`OK`/`SKIP`/`FATAL`).
- Integration into existing unit-test framework.

Everything after M1 is adding providers behind the same interface. If M1 lands cleanly and feels right in code review, the rest is mechanical.

**Recommended execution order after this plan is approved:**

1. Probes A + B + C (~2 hours, non-code).
2. Milestone 1 (~1 day) — **reviewable as a PoC PR on its own** if wanted.
3. If M1 review goes well → proceed M2 through M8 as a single larger PR, or split into M3 / M4+5 / M6+7+8 if reviewers prefer smaller chunks.
