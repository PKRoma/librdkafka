# AWS GetWebIdentityToken — End-to-End Flow on ECS, Fargate, and EKS

**Audience:** anyone reviewing, debugging, or extending the ECS (M4) and web_identity (M5) providers in [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c).

**Scope:** what happens, byte by byte, when our C code obtains an AWS-signed JWT on ECS / Fargate / EKS Pod Identity / EKS IRSA. Companion to [FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md](FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md), which covers EC2 — refer to that doc for stages 2–7 (SigV4 signing, STS call, response parsing, JWT structure). Those stages are **bit-identical** across all platforms once credentials are obtained; only Stage 1 (credential acquisition) differs.

See also: [DESIGN_AWS_OAUTHBEARER_V1.md](DESIGN_AWS_OAUTHBEARER_V1.md) for the V1 plan.

---

## 1. Big picture — contrast with EC2

```
EC2:   process/container ──► IMDSv2 (169.254.169.254)
                                │
                                │ three-call dance: PUT session token,
                                │ GET role name, GET creds
                                ▼
                       AWS temp credentials


ECS:   container          ──► ECS task-agent on host (169.254.170.2)
                                │
                                │ single GET; agent auths by source IP
                                │ + unguessable per-task UUID path
                                ▼
                       AWS temp credentials


Fargate: container        ──► ECS task-agent (same 169.254.170.2 contract,
                                hosted by the Fargate platform — no EC2
                                host you can see)
                                ▼
                        AWS temp credentials


EKS IRSA: pod            ──► AWS STS directly
                                │
                                │ POST AssumeRoleWithWebIdentity
                                │ with the k8s-projected SA token as proof
                                ▼
                          AWS temp credentials


EKS Pod Identity: pod    ──► Pod Identity agent on node (169.254.170.23)
                                │
                                │ single GET; agent auths by bearer token
                                │ from a rotating file in the pod fs
                                ▼
                        AWS temp credentials
```

Three observations to carry through this document:

1. **No IMDSv2 three-call dance on any container platform.** ECS / Fargate / Pod Identity all do a single HTTP GET. IRSA does one POST to STS. The PUT-session-token gymnastics only apply to EC2.
2. **JSON response shape is identical** to IMDSv2's role-creds endpoint for ECS / Fargate / Pod Identity (all go through our M4 provider). IRSA is different — it gets back XML from STS — but ends up at the same `rd_kafka_aws_credentials_t` struct.
3. **Once credentials are in hand, stages 2–7 are bit-identical to EC2.** The `CURLOPT_AWS_SIGV4`-signed POST to `sts.<region>.amazonaws.com`, the XML response, the JWT — see [FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md](FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md) Stages 2–8 for the full walkthrough. This document only covers the credential-acquisition half.

---

## 2. ECS (on EC2) — classic task role

```
┌────────────────────────── EC2 host instance ────────────────────────────┐
│                                                                          │
│  ecs-agent (daemon on the host; talks to ECS control plane)              │
│    - intercepts 169.254.170.2 via iptables/NAT                           │
│    - caches temp creds for each running task's IAM role                  │
│                                                                          │
│  ┌─── container (your task) ─────────────────────────────────────────┐  │
│  │                                                                    │  │
│  │  env injected at container start:                                  │  │
│  │    AWS_CONTAINER_CREDENTIALS_RELATIVE_URI=/v2/credentials/<uuid>   │  │
│  │                                                                    │  │
│  │  ecs_provider_resolve():                                           │  │
│  │    GET http://169.254.170.2/v2/credentials/<uuid>                  │  │
│  │    ► intercepted by ecs-agent on the host                          │  │
│  │    ◄ 200 OK + JSON { AccessKeyId, SecretAccessKey, Token,          │  │
│  │                      Expiration, RoleArn }                         │  │
│  │                                                                    │  │
│  │  → rd_kafka_aws_credentials_t                                      │  │
│  │                                                                    │  │
│  │  (identical to EC2 Stages 2-7 after this point)                    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Where do the env vars come from?

The ECS task definition declares an IAM role via `taskRoleArn`. When ECS launches your task:

1. The ECS control plane generates a per-task credential ID (a UUID).
2. The ECS agent on the host stashes temporary credentials for the task's IAM role, keyed by that UUID.
3. When the agent starts your container, it injects `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI=/v2/credentials/<uuid>` into the container's environment.
4. The container's `169.254.170.2` is network-intercepted (by Docker network config or the ECS task networking stack) and routed to the ECS agent's localhost listener.

From inside the container, `169.254.170.2` looks like any other host. The UUID is handed to you via the env var — you never see it beforehand.

### 2.2 The wire

One GET, no auth header, no session-token dance.

```http
GET /v2/credentials/9c1e05c8-3e4e-4d39-89c0-... HTTP/1.1
Host: 169.254.170.2
User-Agent: ...
Accept: */*
```

Response:

```http
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: ...

{
  "AccessKeyId":     "ASIA...",
  "SecretAccessKey": "wJalr...",
  "Token":           "IQoJb3JpZ2luX2...",
  "Expiration":      "2026-04-21T13:00:00Z",
  "RoleArn":         "arn:aws:iam::123456789012:role/ecs-task-role"
}
```

Parsed by `ecs_provider_resolve()` → `aws_creds_parse_json_response()` in [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c). Same JSON shape as IMDSv2.

### 2.3 Why no `Authorization` header? Isn't that insecure?

The ECS agent authenticates requests by **source IP**. Only processes sitting in the container's network namespace can originate traffic that appears to come from that container's IP. Combined with the unguessable UUID in the path, this forms a capability-style permission:

- A process in container A asking for container A's credentials → allowed.
- A process in container B asking for container A's credentials → blocked (different source IP; the UUID path matches a different task).
- Anything from outside the host entirely → can't reach `169.254.170.2` at all; it's link-local.

The UUID acts as an unguessable capability that only the ECS agent and the intended container know.

### 2.4 Our code

- [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) `ecs_provider_resolve()` detects `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI`, prepends `http://169.254.170.2`, issues a `GET` with **no** `Authorization` header.
- Unit test `ut_ecs_full_uri_no_auth` asserts the no-auth-header behaviour on the wire.

---

## 3. Fargate — same contract, different platform

Fargate is serverless ECS. From the container's perspective, **it is identical** to ECS:

- Same `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` env var.
- Same `http://169.254.170.2/v2/credentials/<uuid>` endpoint.
- Same JSON response shape.

What's different (and invisible to our code):

- No EC2 host underneath — AWS runs your task on Fargate-managed compute.
- The `169.254.170.2` interception is implemented by Fargate's network plumbing rather than an ECS agent daemon on an EC2 instance.
- Your task shares no hardware with tasks from other AWS accounts.

**Our code handles Fargate with zero branches** — the ECS provider's RELATIVE_URI path is used verbatim. The same `ut_ecs_full_uri_no_auth` coverage applies (the mock can't distinguish ECS from Fargate; the contract is the same).

---

## 4. EKS Pod Identity — the token-file model

EKS has two mechanisms for giving pods AWS credentials. Pod Identity (GA 2023) uses the **same ECS provider code** but in FULL_URI mode. IRSA (older, §5 below) uses our separate web_identity provider.

```
┌─────────────────────── EKS worker node ─────────────────────────────────┐
│                                                                          │
│  Pod Identity agent (DaemonSet, one per node)                            │
│    - listens on 169.254.170.23                                           │
│    - mints per-pod bearer tokens, writes them to rotating files          │
│      mounted into each authorised pod's filesystem                       │
│                                                                          │
│  ┌─── pod (your container) ──────────────────────────────────────────┐  │
│  │                                                                    │  │
│  │  env injected by the Pod Identity admission webhook:               │  │
│  │    AWS_CONTAINER_CREDENTIALS_FULL_URI=http://169.254.170.23/v1/... │  │
│  │    AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE=/var/run/secrets/.../    │  │
│  │                                                                    │  │
│  │  volume mount:                                                     │  │
│  │    /var/run/secrets/pods.eks.amazonaws.com/serviceaccount/token    │  │
│  │      ← file updated in-place by the agent on rotation              │  │
│  │                                                                    │  │
│  │  ecs_provider_resolve() (FULL_URI branch):                         │  │
│  │    1. read token file → strip trailing whitespace                  │  │
│  │    2. host = extract from FULL_URI → check against allowlist       │  │
│  │    3. GET <FULL_URI> with Authorization: <token-from-file>         │  │
│  │    ◄ 200 OK + JSON { AccessKeyId, SecretAccessKey, Token, ... }    │  │
│  │                                                                    │  │
│  │  → rd_kafka_aws_credentials_t                                      │  │
│  │                                                                    │  │
│  │  (identical to EC2 Stages 2-7 after this point)                    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 4.1 Key differences from ECS/Fargate

| | ECS / Fargate | EKS Pod Identity |
|---|---|---|
| Env var | `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` | `AWS_CONTAINER_CREDENTIALS_FULL_URI` |
| Endpoint | Fixed: `http://169.254.170.2/<path>` | Configurable: `http://169.254.170.23/v1/credentials` (or IPv6 `[fd00:ec2::23]`) |
| Authentication to the agent | Source IP + unguessable path UUID | Bearer token in `Authorization` header |
| Token source | — | File: `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE` |
| Token rotation | — | Agent rewrites the file in-place every ~15 min |

### 4.2 The wire

```http
GET /v1/credentials HTTP/1.1
Host: 169.254.170.23
Authorization: <the 1500-byte-ish token read from the file>
Accept: */*
```

Response: same JSON shape as ECS.

### 4.3 Why the token file, and why re-read it every resolve?

- **Source-IP access control** works poorly on Kubernetes. Pods in the same node can share network namespaces depending on CNI plugin, and the trust model for Pod Identity wanted cleaner per-pod isolation.
- **Bearer token** in the `Authorization` header is the answer: each pod has its own token, written to a file that only that pod can read (via Kubernetes volume mount permissions).
- **Rotation in place.** The agent rewrites the token file contents periodically. Caching the token in memory at provider-creation time would go stale. Our `aws_read_token_file()` re-reads on every `resolve()` call to handle this.

### 4.4 Our code

- Same `ecs_provider_resolve()` function. The FULL_URI branch enables:
  - Host allowlist check (`169.254.170.2`, `169.254.170.23`, `[fd00:ec2::23]`, loopbacks). This is a **security boundary** — tested in `ut_url_host_allowed`, including the `http://trusted@evil.com/` userinfo attack.
  - Token file read (`aws_read_token_file()`) → trim trailing whitespace.
  - `Authorization: <token>` header attached to the GET.
- `ut_ecs_full_uri_token_file` asserts on the wire: token file wins over static `AWS_CONTAINER_AUTHORIZATION_TOKEN`, trailing `\n` stripped, correct header.
- `aws_read_token_file()` is **shared with the web_identity provider (M5)** — EKS IRSA also reads a rotating token file, so the trimming logic is unified.

---

## 5. EKS IRSA — direct-to-STS model

EKS IRSA (IAM Roles for Service Accounts, 2019) is the older EKS-to-AWS mechanism. Unlike Pod Identity, it doesn't have a node-local agent; the pod does the STS round-trip itself.

```
┌─────────────────────── EKS worker node ─────────────────────────────────┐
│                                                                          │
│  ┌─── pod (your container) ──────────────────────────────────────────┐  │
│  │                                                                    │  │
│  │  env injected by the EKS mutating webhook:                         │  │
│  │    AWS_WEB_IDENTITY_TOKEN_FILE=/var/run/secrets/eks.../token       │  │
│  │    AWS_ROLE_ARN=arn:aws:iam::...:role/<role>                       │  │
│  │    AWS_ROLE_SESSION_NAME=... (optional)                            │  │
│  │                                                                    │  │
│  │  volume mount:                                                     │  │
│  │    /var/run/secrets/eks.amazonaws.com/serviceaccount/token         │  │
│  │      ← OIDC JWT signed by the k8s API server,                      │  │
│  │        rotated by the kubelet every ~24 h                          │  │
│  │                                                                    │  │
│  │  web_identity_provider_resolve():                                  │  │
│  │    1. read token file → strip trailing whitespace                  │  │
│  │    2. generate session name if AWS_ROLE_SESSION_NAME unset         │  │
│  │    3. POST sts.<region>.amazonaws.com/  (unauthenticated)          │  │
│  │       Action=AssumeRoleWithWebIdentity&RoleArn=...                 │  │
│  │       &RoleSessionName=...&WebIdentityToken=<JWT>                  │  │
│  │    ◄ 200 OK + XML { <Credentials>...</Credentials> }               │  │
│  │                                                                    │  │
│  │  → rd_kafka_aws_credentials_t                                      │  │
│  │                                                                    │  │
│  │  (identical to EC2 Stages 2-7 after this point)                    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 5.1 What makes IRSA different

- **No node-side agent.** The trust is established via the Kubernetes OIDC issuer, not via a privileged daemon on the node.
- **The token file is an OIDC JWT**, signed by the Kubernetes API server. It's structured; the `sub` claim encodes the service account (`system:serviceaccount:<ns>:<sa>`). AWS validates the JWT against a pre-configured OIDC trust in IAM for that EKS cluster.
- **The pod talks directly to AWS STS** using `sts:AssumeRoleWithWebIdentity`. This is one of the very few STS actions that accepts **unauthenticated** requests — no SigV4 signature, no `Authorization` header. The JWT is the proof of identity.
- **The response is XML**, not JSON, because it's classic STS query-protocol.

### 5.2 The wire

```http
POST / HTTP/1.1
Host: sts.<region>.amazonaws.com
Content-Type: application/x-www-form-urlencoded
Content-Length: ...

Action=AssumeRoleWithWebIdentity&Version=2011-06-15
&RoleArn=arn%3Aaws%3Aiam%3A%3A...%3Arole%2F<role>
&RoleSessionName=librdkafka-<timestamp>
&WebIdentityToken=eyJhbGc...<entire JWT>...
```

Note:

- **No `Authorization` header** (confirmed live in Probe C — `auth_type: none`).
- **No SigV4 signature.** The JWT is the authentication.
- **URL-encoded body.** `:` and `/` in the role ARN become `%3A` and `%2F`; the JWT itself is URL-safe base64url but contains `.` separators that pass through.

Response:

```xml
HTTP/1.1 200 OK
Content-Type: text/xml

<AssumeRoleWithWebIdentityResponse ...>
  <AssumeRoleWithWebIdentityResult>
    <Credentials>
      <AccessKeyId>ASIA...</AccessKeyId>
      <SecretAccessKey>wJalr...</SecretAccessKey>
      <SessionToken>IQoJ...</SessionToken>    <!-- note: SessionToken, not Token -->
      <Expiration>2026-04-21T13:00:00Z</Expiration>
    </Credentials>
    <SubjectFromWebIdentityToken>system:serviceaccount:ns:sa</SubjectFromWebIdentityToken>
    <AssumedRoleUser>
      <Arn>arn:aws:sts::...:assumed-role/...</Arn>
      <AssumedRoleId>AROA:...</AssumedRoleId>
    </AssumedRoleUser>
  </AssumeRoleWithWebIdentityResult>
  <ResponseMetadata><RequestId>...</RequestId></ResponseMetadata>
</AssumeRoleWithWebIdentityResponse>
```

**Field name gotcha:** the XML uses `<SessionToken>`, not `<Token>` (which is what the IMDS/ECS JSON uses). Our parser in [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c) extracts by the XML name directly via `rd_aws_xml_extract()`; we don't reuse `aws_creds_parse_json_response()` for this path.

### 5.3 Our code

- `rd_kafka_aws_sts_assume_role_with_web_identity()` in [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c) — unauthenticated POST, XML response, direct field extraction.
- `web_identity_provider_resolve()` in [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) — reads the JWT file, generates a session name if not provided, invokes the STS function.
- Tests: `ut_assume_role_with_web_identity_mock_happy` / `_error`, `ut_web_identity_happy_path`, `ut_web_identity_autogenerated_session_name`, `ut_web_identity_missing_token_file_fatal`, `ut_web_identity_unset_skips`.

---

## 6. Compare: EKS IRSA vs EKS Pod Identity

Same cluster, same goal, two completely different trust models:

| | IRSA (2019) | Pod Identity (2023) |
|---|---|---|
| Env vars | `AWS_WEB_IDENTITY_TOKEN_FILE` + `AWS_ROLE_ARN` | `AWS_CONTAINER_CREDENTIALS_FULL_URI` + `AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE` |
| What the token is | OIDC JWT signed by the k8s API server | Opaque bearer token issued by the Pod Identity agent |
| Where the token file lives | `/var/run/secrets/eks.amazonaws.com/serviceaccount/token` | `/var/run/secrets/pods.eks.amazonaws.com/serviceaccount/token` |
| Trust configuration | IAM OIDC identity provider points at the EKS cluster's OIDC issuer; role trust policy allows the projected SA token | Pod Identity associations managed server-side in the EKS/IAM control plane |
| Exchange mechanism | Pod → **AWS STS** (`AssumeRoleWithWebIdentity`) | Pod → **Pod Identity agent on node** (no STS call from the pod) |
| Provider in our code | **web_identity** (M5) | **ecs** (M4, FULL_URI mode) |
| Network hops from pod | pod → AWS STS (regional endpoint over the internet / VPC endpoint) | pod → local agent on node |
| Depends on a node-level daemon? | no | yes |

Both end at the same `rd_kafka_aws_credentials_t`. Both trigger the same GetWebIdentityToken flow afterwards. But internally, the code paths are entirely separate.

---

## 7. Unified view: after credentials, everything is the same

Once `rd_kafka_aws_credentials_t { AccessKeyId, SecretAccessKey, Token }` is populated — no matter which provider produced it — the GetWebIdentityToken flow is **bit-identical** to [FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md](FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md) Stages 2–7:

- `CURLOPT_AWS_SIGV4 = "aws:amz:<region>:sts"` → libcurl canonicalises + SigV4-signs.
- POST to `https://sts.<region>.amazonaws.com/` with `X-Amz-Security-Token: <Token>`.
- AWS STS validates signature, evaluates IAM policy, returns XML with a JWT.
- Our XML parser extracts `<WebIdentityToken>` + `<Expiration>`.

The `sub` claim in the final JWT reflects whichever IAM role the caller's credentials belong to — ECS task role / Fargate task role / EKS pod role / EC2 instance role. But the wire shape and our code path are identical.

---

## 8. Platform → provider → endpoint reference

| Platform | Env vars set by platform | Provider | Hit |
|---|---|---|---|
| **Lambda** | `AWS_ACCESS_KEY_ID` + `_SECRET_ACCESS_KEY` + `_SESSION_TOKEN` | env (M1) | — (credentials pre-populated) |
| **EC2** | (none; read IMDSv2 directly) | imds (M3) | `http://169.254.169.254` |
| **ECS (on EC2)** | `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` | ecs (M4, RELATIVE_URI) | `http://169.254.170.2<path>` |
| **Fargate** | `AWS_CONTAINER_CREDENTIALS_RELATIVE_URI` | ecs (M4, RELATIVE_URI) | `http://169.254.170.2<path>` |
| **EKS IRSA** | `AWS_WEB_IDENTITY_TOKEN_FILE` + `AWS_ROLE_ARN` | web_identity (M5) | AWS STS (`AssumeRoleWithWebIdentity`) |
| **EKS Pod Identity** | `AWS_CONTAINER_CREDENTIALS_FULL_URI` + `..._TOKEN_FILE` | ecs (M4, FULL_URI) | `http://169.254.170.23/v1/credentials` |

After any of these produces credentials, the exact same EC2-flow document describes what happens next.

---

## 9. Source-file index

| File | Purpose |
|---|---|
| [src/rdkafka_aws_credentials.c](src/rdkafka_aws_credentials.c) | `ecs_provider_*` (M4), `web_identity_provider_*` (M5), shared `aws_read_token_file()`, `url_host_allowed()`, unit tests |
| [src/rdkafka_aws_credentials.h](src/rdkafka_aws_credentials.h) | Provider factory prototypes + docstrings describing each platform's semantics |
| [src/rdkafka_aws_sts.c](src/rdkafka_aws_sts.c) | `rd_kafka_aws_sts_assume_role_with_web_identity()` — the unauthenticated STS POST used by IRSA |
| [src/rdkafka_aws_sts.h](src/rdkafka_aws_sts.h) | STS client prototypes |
| [FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md](FLOW_AWS_GETWEBIDENTITYTOKEN_EC2.md) | Companion doc: EC2 flow + shared Stages 2–7 |
| [DESIGN_AWS_OAUTHBEARER_V1.md](DESIGN_AWS_OAUTHBEARER_V1.md) | V1 plan and milestone tracker |
