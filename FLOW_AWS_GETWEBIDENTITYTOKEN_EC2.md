# AWS GetWebIdentityToken — End-to-End Flow on EC2

**Audience:** anyone reviewing, debugging, or extending [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) + [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c).

**Scope:** what happens, byte by byte and header by header, when our C code obtains an AWS-signed JWT on a real EC2 instance — starting from nothing and ending with a `rd_kafka_aws_sts_jwt_t`. Validated end-to-end on 2026-04-21 (account `708975691912`, region `eu-north-1`, role `ktrue-iam-sts-test-role`).

See also: [DESIGN_AWS_OAUTHBEARER_V1.md](DESIGN_AWS_OAUTHBEARER_V1.md) for the V1 plan; this document is the runtime counterpart.

---

## 1. Big picture

```
┌────────────── EC2 instance (ktrue-iam-sts-test-role) ──────────────────┐
│                                                                         │
│  librdkafka (linked into test-runner or app)                            │
│                                                                         │
│  Stage 1 — IMDSv2 "who am I?"                                           │
│    PUT  http://169.254.169.254/latest/api/token                         │
│      → IMDSv2 session token (instance-local, opaque)                    │
│    GET  http://169.254.169.254/latest/meta-data/iam/security-           │
│         credentials/                                                    │
│      → role name ("ktrue-iam-sts-test-role")                            │
│    GET  http://169.254.169.254/latest/meta-data/iam/security-           │
│         credentials/<role-name>                                         │
│      → { AccessKeyId, SecretAccessKey, Token, Expiration }              │
│                                                                         │
│  Stage 2 — Build request                                                │
│    URL:    https://sts.eu-north-1.amazonaws.com/                        │
│    Body:   Action=GetWebIdentityToken&...&Audience.member.1=...         │
│                                                                         │
│  Stage 3/4 — SigV4 sign (libcurl does the crypto)                       │
│    CURLOPT_AWS_SIGV4  = "aws:amz:eu-north-1:sts"                        │
│    CURLOPT_USERPWD    = "<AKID>:<SAK>"                                  │
│    X-Amz-Security-Token: <session token> (via CURLOPT_HTTPHEADER)       │
│                                                                         │
│  Stage 5 — POST over TLS to real AWS STS                                │
│    sts.eu-north-1.amazonaws.com:443                                     │
│                                                                         │
│  Stage 6 — Parse XML response                                           │
│    <WebIdentityToken>eyJ...</WebIdentityToken>                          │
│    <Expiration>2026-04-21T07:37:10.000Z</Expiration>                    │
│                                                                         │
│  → rd_kafka_aws_sts_jwt_t { token, expiration_us }                      │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Stage 1 — Acquire temporary AWS credentials from IMDSv2

An EC2 instance with an attached IAM role can fetch temporary credentials for that role from the link-local Instance Metadata Service at `169.254.169.254`. No long-lived secrets need to be baked into the instance.

IMDSv2 requires a **three-call dance**. Our code in `imds_provider_resolve()` at [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) executes all three.

### 2.1 Get an IMDSv2 session token (`imds_fetch_session_token`)

```http
PUT /latest/api/token HTTP/1.1
Host: 169.254.169.254
X-aws-ec2-metadata-token-ttl-seconds: 300
Content-Length: 0
```

**Response (HTTP 200):**
```
AQAEAP...                          ← ~56-char opaque token, valid 300 s
```

**Purpose:** SSRF protection. IMDSv2 demands the caller can send a PUT with a custom header before it will hand out any metadata. Most Server-Side Request Forgery vulnerabilities can only force a vulnerable service to issue GETs, so this simple requirement kills the vast majority of SSRF-to-credential-exfiltration attacks that plagued IMDSv1.

This token is entirely IMDS-local. It has **nothing to do with AWS IAM or STS**.

### 2.2 Discover the role name (`imds_get` call #1)

```http
GET /latest/meta-data/iam/security-credentials/ HTTP/1.1
Host: 169.254.169.254
X-aws-ec2-metadata-token: AQAEAP...
```

**Response:**
```
ktrue-iam-sts-test-role
```

Plain text, one line. Each instance profile has exactly one role, so this always returns a single name.

### 2.3 Fetch the temporary credentials (`imds_get` call #2)

```http
GET /latest/meta-data/iam/security-credentials/ktrue-iam-sts-test-role HTTP/1.1
Host: 169.254.169.254
X-aws-ec2-metadata-token: AQAEAP...
```

**Response (JSON):**
```json
{
  "Code": "Success",
  "LastUpdated": "2026-04-21T05:15:00Z",
  "Type": "AWS-HMAC",
  "AccessKeyId": "ASIA2KER6RCE...",
  "SecretAccessKey": "wJalr...(40 chars)...",
  "Token": "IQoJb3JpZ2luX2VjE...(~1300 chars)...",
  "Expiration": "2026-04-21T13:34:35Z"
}
```

These are **AWS-level temporary credentials**. Not to be confused with the IMDSv2 session token from 2.1.

| Field | Role |
|---|---|
| `AccessKeyId` | Public half. `ASIA` prefix = temporary; long-lived IAM-user keys start with `AKIA`. |
| `SecretAccessKey` | Private half. Used to derive the SigV4 signing key. Never transmitted. |
| `Token` | Opaque AWS session token. Must accompany every SigV4-signed request in the `X-Amz-Security-Token` header. Proves these credentials are session-scoped to this instance's role. |
| `Expiration` | Wall-clock time when these credentials stop working. Typically ~6 hours out. |

Our parser (`imds_parse_creds_json`) converts `Expiration` from ISO 8601 UTC to an `rd_clock()`-basis monotonic microsecond timestamp so subsequent expiry checks are clock-jump-safe.

### Why plain HTTP (not HTTPS) for IMDS?

All three calls are over `http://169.254.169.254`. This is fine because:
- The traffic is confined to the instance's link-local network (169.254.0.0/16). It never leaves the hypervisor.
- TLS adds cost; IMDS is hit frequently during credential rotation.
- IMDSv2's session-token requirement already prevents SSRF abuse.

---

## 3. Stage 2 — Prepare the STS call

Control flows into `rd_kafka_aws_sts_get_web_identity_token()` at [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c). Inputs:

- Credentials from Stage 1 (`AccessKeyId`, `SecretAccessKey`, `Token`)
- `region = "eu-north-1"`
- `audience = "https://api.example.com"`
- `signing_algorithm = "RS256"`
- `duration_seconds = 300`

### 3.1 Endpoint URL

```
https://sts.eu-north-1.amazonaws.com/
```

Built in `build_sts_endpoint_url()`. No path, no query string — classic STS Query protocol puts everything in the request body.

**GetWebIdentityToken is NOT available on the STS global endpoint** (`sts.amazonaws.com`). AWS requires a regional endpoint for this API. Our code takes `region` as a required argument and never silently defaults.

The `AWS_STS_ENDPOINT_URL` env var is also honoured (tests redirect to a mock; deployments may use FIPS or VPC endpoints).

### 3.2 Request body (form-urlencoded)

Built in `build_request_body()`:

```
Action=GetWebIdentityToken
&Version=2011-06-15
&Audience.member.1=https%3A%2F%2Fapi.example.com
&SigningAlgorithm=RS256
&DurationSeconds=300
```

Notes:

- `Audience.member.1` is AWS's form-encoding for a list-of-one. For multi-audience tokens we would add `.member.2`, `.member.3`, etc.
- Audience is URL-encoded via `curl_easy_escape()` because `:` and `/` are URL-reserved characters.
- `Version=2011-06-15` is the historical STS API version string — required, not meaningful to us.

---

## 4. Stage 3 — The HTTP request we intend to send

Before libcurl signs anything, here is the shape of the request that libcurl will then sign. libcurl adds the `Host`, `Content-Length`, `X-Amz-Date`, and `Authorization` headers itself; we supply the rest:

```http
POST / HTTP/1.1
Host: sts.eu-north-1.amazonaws.com                 ← added by libcurl
Content-Type: application/x-www-form-urlencoded    ← we add via CURLOPT_HTTPHEADER
X-Amz-Security-Token: IQoJb3JpZ2luX2VjE...         ← we add (from IMDS)
Expect:                                            ← empty to suppress 100-continue
Content-Length: 163                                ← added by libcurl
X-Amz-Date: 20260421T073210Z                       ← added by libcurl (current UTC)
Authorization: AWS4-HMAC-SHA256 ...                ← computed by libcurl (Stage 4)

Action=GetWebIdentityToken&Version=2011-06-15&Audience.member.1=https%3A%2F%2Fapi.example.com&SigningAlgorithm=RS256&DurationSeconds=300
```

Relevant libcurl options, set in [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c):

```c
curl_easy_setopt(hreq.hreq_curl, CURLOPT_AWS_SIGV4, "aws:amz:eu-north-1:sts");
curl_easy_setopt(hreq.hreq_curl, CURLOPT_USERPWD,   "ASIA...:wJalr...");
headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
headers = curl_slist_append(headers, "Expect:");
headers = curl_slist_append(headers, "X-Amz-Security-Token: IQoJ...");
curl_easy_setopt(hreq.hreq_curl, CURLOPT_HTTPHEADER, headers);
curl_easy_setopt(hreq.hreq_curl, CURLOPT_POST,          1L);
curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDS,    body);
curl_easy_setopt(hreq.hreq_curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
```

That is the entirety of our "SigV4 signing code." libcurl does everything in Stage 4 for us.

**`Expect:` (empty)** suppresses the default `Expect: 100-continue` header that libcurl would otherwise add for POST/PUT requests over a size threshold. Without suppression, libcurl pauses ~1 s waiting for a `100 Continue` before sending the body.

---

## 5. Stage 4 — SigV4 signing, inside libcurl

When `curl_easy_perform()` runs, libcurl implements AWS Signature Version 4 using OpenSSL's HMAC-SHA256 primitives. Here is what it does (for reference — our code does not invoke any of this directly).

### 5.1 Canonical request

libcurl builds a strictly-formatted string that represents the request in a way both sides can reconstruct byte-for-byte:

```
POST
/

content-type:application/x-www-form-urlencoded
host:sts.eu-north-1.amazonaws.com
x-amz-date:20260421T073210Z
x-amz-security-token:IQoJ...

content-type;host;x-amz-date;x-amz-security-token
<SHA256-hex of the request body>
```

Line-by-line format:

1. HTTP method.
2. URI path (or `/` if empty).
3. Canonical query string (empty for POSTs with form body).
4. One line per signed header (lowercased name, colon, trimmed value), sorted alphabetically, each terminated with `\n`.
5. Blank line.
6. Semicolon-joined list of the signed header names (same order).
7. Lowercase hex of SHA-256 of the request body.

Client and server must produce **byte-identical** canonical strings. That property is the entire foundation of signature verification.

**Probe A (Phase 0) confirmed that libcurl includes our manually-added `X-Amz-Security-Token` header in this canonical request.** Without that behaviour, temporary credentials from IMDSv2 would be unusable with `CURLOPT_AWS_SIGV4`.

### 5.2 String to sign

```
AWS4-HMAC-SHA256
20260421T073210Z
20260421/eu-north-1/sts/aws4_request
<SHA256-hex of canonical request>
```

The third line is the **credential scope** — date, region, service, and the literal `aws4_request`. It narrows a derived signing key to a specific day + region + service, so a leaked signing key cannot be reused across scopes.

### 5.3 Derive the signing key (four HMAC-SHA256 operations)

```
kSecret  = "AWS4" + SecretAccessKey
kDate    = HMAC-SHA256(kSecret,  "20260421")
kRegion  = HMAC-SHA256(kDate,    "eu-north-1")
kService = HMAC-SHA256(kRegion,  "sts")
kSigning = HMAC-SHA256(kService, "aws4_request")
```

OpenSSL's `HMAC()` does each step. The `SecretAccessKey` appears only here; it never goes on the wire.

### 5.4 Final signature

```
signature = hex-encode( HMAC-SHA256(kSigning, string_to_sign) )
```

64 lowercase hex characters.

### 5.5 Authorization header

```
Authorization: AWS4-HMAC-SHA256 Credential=ASIA.../20260421/eu-north-1/sts/aws4_request, SignedHeaders=content-type;host;x-amz-date;x-amz-security-token, Signature=<64 hex chars>
```

libcurl attaches this header and transmits the request over the already-negotiated TLS connection.

---

## 6. Stage 5 — AWS STS processes the request

Server-side, at `sts.eu-north-1.amazonaws.com`:

1. **TLS handshake** completes (libcurl ↔ STS, using OpenSSL with the system CA trust store).
2. **Principal lookup.** AWS parses the `AccessKeyId` from the `Credential=` component of `Authorization`, looks up the active session, and retrieves the role's `SecretAccessKey` + the `Token`.
3. **Signature verification.** STS rebuilds the canonical request using the received method, headers (those listed in `SignedHeaders`), and body. Derives its own `kSigning`. Recomputes the signature. Must byte-equal the transmitted `Signature=`. If not → `SignatureDoesNotMatch`.
4. **Token verification.** `X-Amz-Security-Token` must match the session token AWS issued for this role instance. If stale → `InvalidClientTokenId` / `ExpiredToken`.
5. **Clock-skew check.** `X-Amz-Date` must fall within ±5 minutes of AWS's wall-clock. If off → `RequestTimeTooSkewed`.
6. **IAM policy evaluation** for `sts:GetWebIdentityToken`:
   - Identity-based policy attached to the role must Allow the action.
   - Any `Condition` block (e.g. `sts:IdentityTokenAudience`) must evaluate true against the requested audience.
   - Audience mismatch surfaces as `AccessDenied` "no identity-based policy allows ..." — our [ut_sts_real](src/rdkafka_aws_credentials.c) hit exactly this during early testing.
7. **Account-level precondition.** Outbound Web Identity Federation must be enabled on the AWS account. One-time admin action via `aws iam enable-outbound-web-identity-federation`.
8. **JWT generation.**
   - AWS builds three claim sections: `header`, `payload`, `signature`.
   - Signs `base64url(header) + "." + base64url(payload)` with AWS's private RSA key (for `RS256`) or ECDSA P-384 key (for `ES384`).
   - Public verification keys are published at the account-specific issuer URL's JWKS endpoint: `https://<account-uuid>.tokens.sts.global.api.aws/.well-known/jwks.json`.

Typical end-to-end latency: 20–100 ms for signature verification + policy evaluation + JWT mint.

---

## 7. Stage 6 — The response

STS returns classic STS Query-protocol XML with `Content-Type: text/xml`:

```xml
HTTP/1.1 200 OK
Content-Type: text/xml
x-amzn-RequestId: c9c99a1c-5763-4db3-87c4-...

<?xml version="1.0" encoding="UTF-8"?>
<GetWebIdentityTokenResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
  <GetWebIdentityTokenResult>
    <WebIdentityToken>eyJraWQiOiJSU0FfMSIsInR5cCI6IkpXVCIsImFsZyI6IlJTMjU2In0.eyJhdWQiOi...(~1400 bytes)...</WebIdentityToken>
    <Expiration>2026-04-21T07:37:10.000Z</Expiration>
  </GetWebIdentityTokenResult>
  <ResponseMetadata>
    <RequestId>c9c99a1c-5763-4db3-87c4-...</RequestId>
  </ResponseMetadata>
</GetWebIdentityTokenResponse>
```

Error responses (HTTP 4xx/5xx) have a different envelope:

```xml
<ErrorResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
  <Error>
    <Type>Sender</Type>
    <Code>AccessDenied</Code>
    <Message>...</Message>
  </Error>
  <RequestId>...</RequestId>
</ErrorResponse>
```

---

## 8. Stage 7 — Our response parser

Back in [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c), after `curl_easy_perform()` returns:

1. **`read_response_body()`** copies the HTTP body out of libcurl's `rd_buf_t` into a NUL-terminated C string.
2. **`rd_aws_xml_extract(body, "WebIdentityToken")`** — a 20-line substring-based extractor that returns the content between `<WebIdentityToken>` and `</WebIdentityToken>`. Returns `NULL` if not found.
3. **`rd_aws_xml_extract(body, "Expiration")`** — same, for the expiration timestamp string.
4. **`rd_aws_parse_iso8601_utc(expiration_str, &epoch)`** — portable ISO 8601 / RFC 3339 parser. Accepts `Z`, `+HH:MM`, `+HHMM`, `-HH:MM`, with optional fractional seconds.
5. **`rd_aws_epoch_to_monotonic_us(epoch)`** — converts wall-clock epoch seconds to `rd_clock()`-basis monotonic microseconds. Computes the delta at translation time so the result is immune to wall-clock jumps.
6. **Construct `rd_kafka_aws_sts_jwt_t { token, expiration_us }`** and return.

On error:

1. `format_sts_error(http_code, xml_body, errstr)` extracts `<Code>` and `<Message>` from the `<ErrorResponse>` envelope and composes a human-readable error string.
2. Return `RD_KAFKA_RESP_ERR__AUTHENTICATION` (or `__TRANSPORT` for pre-HTTP failures).

**No libxml2, no regex engine, no generic JSON parser for the STS wire format.** Just `strstr`, `memcpy`, and `sscanf`.

---

## 9. The JWT itself (informational)

librdkafka treats the JWT as opaque — it forwards the token to the external OIDC service via OAUTHBEARER SASL and does not inspect claims. For completeness, here is the structure, observed on the 1467-byte token from the `ut_sts_real` run.

A JWT is three base64url-encoded sections joined with `.`:

```
<base64url(header)>.<base64url(payload)>.<base64url(signature)>
```

**Header** (base64url-decoded JSON):

```json
{ "kid": "RSA_1", "typ": "JWT", "alg": "RS256" }
```

`kid` tells the consumer which public key to fetch from the issuer's JWKS endpoint.

**Payload** (base64url-decoded JSON):

```json
{
  "aud": "https://api.example.com",
  "sub": "arn:aws:iam::708975691912:role/ktrue-iam-sts-test-role",
  "iss": "https://a1ebc705-cd4d-42b4-93b5-96e93acfb431.tokens.sts.global.api.aws",
  "iat": 1776751307,
  "exp": 1776751607,
  "jti": "c9c99a1c-5763-4db3-87c4-b96d4749b0d0",
  "https://sts.amazonaws.com/": {
    "aws_account":       "708975691912",
    "source_region":     "eu-north-1",
    "org_id":            "o-0x3t8umolz",
    "ou_path":           ["o-0x3t8umolz/r-zc5j/..."],
    "ec2_source_instance_arn":          "arn:aws:ec2:eu-north-1:...:instance/i-...",
    "ec2_instance_source_vpc":          "vpc-...",
    "ec2_instance_source_private_ipv4": "172.31.1.146",
    "ec2_role_delivery":                "2.0",
    "principal_id":      "arn:aws:iam::708975691912:role/ktrue-iam-sts-test-role",
    "principal_tags":    { "divvy_owner": "ktrue@confluent.io" }
  }
}
```

Standard OIDC claims (`iss`, `sub`, `aud`, `iat`, `exp`, `jti`) plus AWS-specific nested claims under the `https://sts.amazonaws.com/` key.

**Signature:** 256-byte RSA-SHA256 signature over `base64url(header) + "." + base64url(payload)`, itself base64url-encoded.

The external OIDC service will:

1. Decode header → find `kid`.
2. GET `{iss}/.well-known/openid-configuration` → discover the JWKS URL.
3. GET the JWKS → pick the key matching `kid`.
4. Verify the signature.
5. Validate claims: `iss` matches a trusted issuer, `aud` matches expected audience, `exp` in the future.

---

## 10. Dependencies used (at runtime)

| What ran | Source | New in V1 M3+M7? |
|---|---|---|
| HTTP + TLS to IMDS and STS | libcurl (pre-existing librdkafka dep) | no |
| HMAC-SHA256 / SHA256 | OpenSSL via libcurl (pre-existing librdkafka dep) | no |
| JSON parse (IMDS credentials response) | cJSON (vendored in librdkafka) | no |
| XML extract (STS response) | ~20 lines of C in `rdkafka_aws_sts.c` | yes |
| ISO 8601 / RFC 3339 parse | ~80 lines of C in `rdkafka_aws_credentials.c` | yes |
| In-process mock HTTP server (tests only) | ~200 lines of C in `rdkafka_aws_credentials.c` | yes |
| Everything else | Pure C, stdlib + librdkafka utilities | — |

**No AWS CLI, no aws-sdk-cpp, no aws-c-* libraries, no process spawn, no additional build-time dependencies.** The dependency list of librdkafka is identical before and after this work.

---

## 11. Source-file index

| File | Purpose |
|---|---|
| [src/rdkafka_aws_credentials.h](src/rdkafka_aws_credentials.h) | Credentials struct, provider interface, factories, shared helpers |
| [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) | env + IMDSv2 + chain providers; ISO 8601 parser; unit tests; mock HTTP server |
| [src/rdkafka_aws_sts.h](src/rdkafka_aws_sts.h) | STS client types and prototypes |
| [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c) | `GetWebIdentityToken` implementation; XML extractor; request/response helpers |
| [src/rdhttp.h](src/rdhttp.h), [src/rdhttp.c](src/rdhttp.c) | Generic HTTP GET/POST/PUT helpers (pre-existing, `rd_http_put()` added for IMDSv2) |
| [DESIGN_AWS_OAUTHBEARER_V1.md](DESIGN_AWS_OAUTHBEARER_V1.md) | V1 plan and milestone tracker |

## 12. Reproducing end-to-end on EC2

```bash
# Configure (from repo root, once):
./configure

# Build library + test-runner:
make -C src -j
make -C src-cpp -j
make -C tests build -j

# Run just the aws_credentials unit tests with real AWS enabled:
cd tests
LD_LIBRARY_PATH=../src:../src-cpp \
  TESTS=0000 RD_UT_TEST=aws_credentials \
  RD_UT_IMDS=1 RD_UT_STS=1 RD_UT_AWS_REGION=eu-north-1 \
  ./test-runner -l
```

Expected tail of output:

```
RDUT: PASS: ... ut_imds_real (IMDSv2 resolved real credentials: AKID prefix=ASIA***, ...)
RDUT: PASS: ... ut_sts_real  (STS GetWebIdentityToken success: JWT=NNNN bytes, ...)
ALL TESTS PASSED
```

If your IAM policy restricts the audience (via `sts:IdentityTokenAudience`), override:

```bash
RD_UT_AWS_AUDIENCE="https://your-policy-allowed-audience.example.com" \
RD_UT_STS=1 RD_UT_AWS_REGION=... \
./test-runner -l
```
