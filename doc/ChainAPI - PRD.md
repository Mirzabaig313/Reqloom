# ChainAPI — Product Requirements Document

> **A workflow-aware API testing tool that auto-resolves request dependency chains, eliminating the manual copy-paste loop developers suffer through with Postman.**

| | |
|---|---|
| **Document Owner** | Mirza |
| **Status** | Draft v0.4 |
| **Last Updated** | 2026-05-24 |
| **Target Release** | MVP — 10 weeks from kickoff |

> **Note**:Examples in this document use a generic **"MarketplaceAPI"** with admin / vendor / customer actors and product / order / refund resources. Any resemblance to a specific real-world API is illustrative only.

---

## 1. Executive Summary

### 1.1 Problem

Modern SaaS backends are **multi-role and deeply interconnected**. To test a single endpoint, developers and QAs often need to:

1. Log in as multiple actors (admin, vendor, customer, manager, etc.)
2. Chain 3–6 API calls just to obtain prerequisite IDs
3. Manually copy tokens and IDs between requests in Postman/Bruno/Insomnia
4. Repeat this every time tokens expire or test data is reset

A representative example — testing **"admin approves a customer refund"** in a marketplace API requires this chain:

```
Admin login            → admin_token
Vendor login           → vendor_token + vendor_id
Customer login         → customer_token + customer_id
Create product         → product_id            (vendor)
Place order            → order_id              (customer)
Pay for order          → payment_id            (customer)
Request refund         → refund_id             (customer)
Approve refund         ← target endpoint       (admin)
```

That's **8 manual requests** with **7 IDs** to copy-paste, just to test one endpoint. Across a feature with 20 endpoints, this becomes hours of mechanical work per test cycle.

### 1.2 Why Existing Tools Fall Short

| Tool | Limitation |
|------|------------|
| **Postman / Postman Flows** | Manual scripting; visual flows require wiring every relationship by hand; no concept of actors with reusable auth |
| **Bruno** | Same scripting model as Postman; git-friendly but no dependency awareness |
| **Insomnia** | Request chaining is manual; no role/actor abstraction |
| **Hurl / Stepci** | Linear scripts; require manually authoring entire workflows for every endpoint |
| **Apidog** | Better than Postman but still requires manual workflow definition; no AI import |

**None of them model the API as a dependency graph.** They treat it as a flat list of requests.

### 1.3 Vision

ChainAPI treats your API as a **graph of resources, actors, and dependencies**. Define each actor (auth flow) and each resource (endpoints + dependencies) **once**. Then click any endpoint and it auto-resolves the entire chain — login, prerequisites, target call — and executes them in the correct order.

Add an **AI doc importer** that reads existing API docs (OpenAPI, Postman collections, Markdown, curl examples) and auto-generates the dependency graph, and the tool goes from zero to "run any endpoint with one click" in under five minutes.

### 1.4 Strategic Positioning

> **"Postman is an HTTP client. ChainAPI is an API workflow engine."**

Wedge: **backend developers and QAs working on multi-role SaaS** (B2B platforms, marketplaces, enterprise admin panels). This is the majority of modern SaaS.

---

## 2. Goals & Non-Goals

### 2.1 Goals (MVP)

- **G1** — Eliminate manual copy-paste of tokens and IDs between requests
- **G2** — Allow any endpoint in a project to be tested in one click, regardless of dependency depth
- **G3** — Make actor/role auth flows a first-class concept (not scripted into every request)
- **G4** — Allow AI-driven import of existing API docs to bootstrap a project in minutes
- **G5** — Allow direct import of Postman/Bruno/Insomnia collections (zero-friction migration)
- **G6** — Be fast (sub-second to render any endpoint), local-first, and privacy-respecting (no forced cloud sync)

### 2.2 Future Goals (Post-MVP)

These are committed product directions but explicitly out of scope for v1. The architectural decision to make is that the **same schema** that powers testing must be reusable for these features — no parallel definitions.

- **G7** — Schema-driven **mock server**: any project's `chainapi.yaml` can be flipped into "serve mode" to expose a local HTTP server returning recorded or templated responses for the defined operations. Useful for frontend development, demoing flows offline, and reproducing edge-case responses (errors, slow responses, malformed payloads).
- **G8** — Schema-driven **API documentation generation**: render `chainapi.yaml` to a browsable static site (HTML/Markdown) that documents every actor, resource, and operation, complete with example request/response pairs captured from real runs.
- **G9** — **Browser-based version**: a hosted web app for testing public/staging APIs without installing the desktop app. Excluded from MVP because browsers cannot reach `localhost` from a remote origin (CORS + mixed-content restrictions), which is the dominant testing scenario for backend devs. Viable later as either (a) a remote runner paired with a tiny local agent that proxies requests, or (b) a hosted version targeted at staging/production API testing only.

### 2.3 Non-Goals (MVP)

- ❌ Replacing Postman for ad-hoc one-off requests (Postman is fine at that)
- ❌ Performance/load testing (k6, Gatling territory)
- ❌ Real-time team collaboration (cursor sharing, comments) — git-based sharing only
- ❌ Cloud-hosted version (local-first; cloud is post-MVP)

---

## 3. Target Users & Personas

### 3.1 Primary Persona — Backend Developer

> **"Riya, 28, Senior Backend Engineer at a 50-person B2B SaaS"**

- Works on a NestJS/Spring/Django backend with 100+ endpoints across 5 modules
- Tests her own endpoints during development — Postman is her daily driver
- Frustrations:
  - Every morning, re-logs in 3 actors because tokens expired overnight
  - Copies UUIDs from one tab to another constantly
  - Has 200+ Postman requests, can't find anything
  - When DB is reset, all her saved variables are stale

**ChainAPI value**: One-click testing of any endpoint. Skip the morning re-login ritual. Stop hunting for IDs.

### 3.2 Secondary Persona — QA Tester

> **"Arjun, 32, QA Lead handling regression tests across web + mobile"**

- Tests features end-to-end across admin, web, and mobile apps
- Today: opens 3 frontends, creates data through UI, tests on each platform
- Frustrations:
  - UI-driven testing is slow; backend bugs take 20 minutes to reproduce
  - Hard to test "what if admin rejects after vendor confirms" without complex UI dance
  - Can't easily reproduce edge cases without dev help

**ChainAPI value**: Run complex multi-actor flows in seconds. Test edge cases without the UI overhead.

### 3.3 Tertiary Persona — Solo Developer / Small Team Tech Lead

> Someone repeatedly building multi-role SaaS products

- Wears all the hats: backend, QA, deployment
- Frustrations:
  - Same testing pain on every project
  - Onboarding new devs takes ages because no one understands the data dependency graph
  - Bug repros require detailed setup steps that everyone forgets

**ChainAPI value**: Reusable schema across projects. Onboarding becomes "import this YAML, run any endpoint."

---

## 4. Core Concepts (Domain Model)

### 4.1 Actor

An identity in the system with its own authentication flow.

- Has a name (`admin`, `vendor`, `customer`)
- Has one or more **Auth Strategies** (login, OTP, OAuth, API key)
- Each strategy is itself a sequence of requests that produce **session variables** (token, user_id, vendor_id)
- Sessions are cached and reused across requests until they expire

### 4.2 Resource

A domain entity exposed via API (`product`, `order`, `refund`).

- Has one or more **Operations** (create, get, list, update, delete, custom actions like `approve`)
- Each operation defines:
  - HTTP method + URL template
  - Required actor (which auth context to use)
  - Request body / query / headers
  - Dependencies on other resources/operations
  - Output extractions (which response fields become variables)

### 4.3 Dependency

A directed edge in the graph: "Operation X requires Operation Y to have run first."

Two types:
- **Implicit**: Variable references (`{{order.order_id}}` implies `order.create` ran)
- **Explicit**: `depends_on: [order.create]` declarations

### 4.4 Environment

A named set of base config values (`baseUrl`, default credentials, feature flags).

- `local`, `staging`, `production`
- Switchable in one click
- Sensitive values stored in OS keychain, not in YAML

### 4.5 Run Context

The runtime state of an execution.

- Tracks which actors are logged in (and their session variables)
- Tracks which resources have been created (and their extracted IDs)
- Persists across runs in the same session for speed (cached but invalidatable)

### 4.6 Project

The top-level container.

- One project = one API
- Contains: actors, resources, environments, schema metadata
- Stored as a folder of YAML files (`chainapi.yaml` + sub-files), git-friendly

---

## 5. The Schema Specification

This is the heart of the product — the format AI agents will generate, that humans will edit, and that the engine consumes.

### 5.1 Minimum Viable Schema

The simplest schema that runs is a single file. The spec is layered: simple cases stay simple, complex cases scale up.

```yaml
# chainapi.yaml — minimum viable schema (single file, single actor, single endpoint)
version: 1
name: My API
environment:
  baseUrl: http://localhost:3000

actors:
  user:
    auth:
      method: POST
      path: /login
      body: { email: "test@test.com", password: "test123" }
      extract: { token: $.accessToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  hello:
    get:
      method: GET
      path: /api/hello
      actor: user
```

That's a valid project. Click `hello.get`, the engine logs the user in, then calls `/api/hello`. Done.

### 5.2 File Structure (for larger projects)

```
my-project/
├── chainapi.yaml              # Project root config
├── environments/
│   ├── local.yaml
│   └── staging.yaml
├── actors/
│   ├── admin.yaml
│   ├── vendor.yaml
│   └── customer.yaml
└── resources/
    ├── products.yaml
    ├── orders.yaml
    └── refunds.yaml
```

### 5.3 Project Root (`chainapi.yaml`)

```yaml
version: 1
name: MarketplaceAPI
description: B2C marketplace backend
default_environment: local
imports:
  - actors/*.yaml
  - resources/*.yaml
```

### 5.4 Environment

```yaml
name: local
variables:
  baseUrl: http://localhost:3000
  admin_email: admin@marketplace.test
  admin_password: !secret ADMIN_PASSWORD   # resolves from OS keychain
  vendor_email: vendor@marketplace.test
  customer_email: customer@marketplace.test
```

### 5.5 Actor Definition

Vendor — email + password (typical employer/admin pattern):

```yaml
name: vendor
description: Marketplace vendor with email/password auth

auth:
  strategy: simple
  method: POST
  path: /api/v1/auth/vendor/login
  body:
    email: "{{env.vendor_email}}"
    password: "{{env.vendor_password}}"
  expect_status: 200
  extract:
    token: $.data.accessToken
    refresh_token: $.data.refreshToken
    vendor_id: $.data.user.id

session:
  ttl: 15m                       # auto re-login after this
  refresh:
    method: POST
    path: /api/v1/auth/refresh
    body: { refresh_token: "{{vendor.refresh_token}}" }
    extract: { token: $.data.accessToken }

inject:
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

Customer — multi-step OTP auth (mobile app pattern):

```yaml
name: customer
description: Mobile app user (OTP-based auth)

auth:
  strategy: chain
  steps:
    - id: send_otp
      method: POST
      path: /api/v1/auth/send-otp
      body:
        phone: "{{env.test_phone}}"
      expect_status: 200

    - id: verify_otp
      method: POST
      path: /api/v1/auth/verify-otp
      body:
        phone: "{{env.test_phone}}"
        otp: "123456"               # local fixed OTP for testing
      expect_status: 200
      extract:
        token: $.data.accessToken
        customer_id: $.data.user.id

session:
  ttl: 30m

inject:
  headers:
    Authorization: "Bearer {{customer.token}}"
```

### 5.6 Resource Definition

```yaml
name: product
description: Vendor-listed products

operations:
  create:
    method: POST
    path: /api/v1/vendors/{{vendor.vendor_id}}/products
    actor: vendor
    body:
      name: "Demo Product"
      price: 999
      stock: 50
    expect_status: 201
    extract:
      product_id: $.data.id

  publish:
    method: POST
    path: /api/v1/products/{{product.product_id}}/publish
    actor: vendor
    depends_on: [product.create]
```

```yaml
name: order

operations:
  create:
    method: POST
    path: /api/v1/orders
    actor: customer
    depends_on: [product.publish]
    body:
      product_id: "{{product.product_id}}"
      quantity: 1
    extract:
      order_id: $.data.id

  pay:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/pay
    actor: customer
    depends_on: [order.create]
    body:
      method: "card"
      token: "tok_test_visa"
    extract:
      payment_id: $.data.payment.id
```

```yaml
name: refund

operations:
  request:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/refunds
    actor: customer
    depends_on: [order.pay]
    body:
      reason: "Item not as described"
    extract:
      refund_id: $.data.id

  approve:
    method: POST
    path: /api/v1/admin/refunds/{{refund.refund_id}}/approve
    actor: admin
    depends_on: [refund.request]
```

### 5.7 Variable Resolution Rules

| Reference | Resolves To |
|-----------|-------------|
| `{{env.baseUrl}}` | Environment variable |
| `{{vendor.token}}` | Vendor actor's session variable |
| `{{order.order_id}}` | Most recent `order` resource's extracted variable |
| `{{order[2].order_id}}` | Indexed (the second order created in this run) |
| `{{secret.STRIPE_KEY}}` | OS keychain entry |
| `{{$.now}}` | Built-in (current ISO timestamp) |
| `{{$.now+5m}}` / `{{$.now-1h}}` | Built-in (relative time — supports `s`, `m`, `h`, `d`) |
| `{{$.uuid}}` | Built-in (generates a UUID v4) |
| `{{$.faker.email}}` | Built-in (fake data — Faker.js style) |
| `{{$.env.HOSTNAME}}` | OS environment variable |
| `{{$.base64.encode(body)}}` / `{{$.base64.decode(...)}}` | Built-in (base64 codec) |
| `{{$.hex.encode(...)}}` / `{{$.hex.decode(...)}}` | Built-in (hex codec) |
| `{{$.hmac.sha256(secret.HMAC_KEY, body)}}` | Built-in (HMAC; `sha1`, `sha256`, `sha512` supported) |
| `{{$.hash.sha256(body)}}` | Built-in (one-shot hash; `md5`, `sha1`, `sha256`, `sha512`) |
| `{{$.jwt.sign({sub: customer.id, exp: $.now+1h}, secret.JWT_KEY)}}` | Built-in (JWT signing — HS256, HS512, RS256) |
| `{{$.url.encode(...)}}` / `{{$.url.decode(...)}}` | Built-in (URL component codec) |
| `{{$.json.stringify(body)}}` | Built-in (canonicalised JSON for signing) |

The intent is that the **built-in function library covers the 80% of cases** where users would otherwise reach for a JS hook (HMAC signing, JWT generation, base64-wrapping, relative timestamps). Hooks (§5.10) remain available for the genuine long tail.

### 5.8 Dependency Resolution Algorithm

1. User requests to run operation `T` (target)
2. Engine builds reverse dependency graph from `T`
3. Topologically sort dependencies
4. For each dependency in order:
   - If it's an actor login and a valid session exists → skip
   - If it's a resource operation already executed in this run → skip (unless `force: true`)
   - Otherwise, execute and capture extractions
5. Execute target operation `T`
6. Display the executed chain in the UI for transparency

**Example**: Running `refund.approve` triggers:

```
1. admin.auth.login         (skipped — session valid)
2. vendor.auth.login        (executed)
3. customer.auth.verify_otp (executed)
4. product.create           (executed)
5. product.publish          (executed)
6. order.create             (executed)
7. order.pay                (executed)
8. refund.request           (executed)
9. refund.approve           (executed — TARGET)
```

### 5.9 Schema Versioning & Evolution

The schema spec itself will evolve. To avoid breaking existing projects:

- Every project YAML declares `version: 1` at the top
- Major version bumps (breaking changes) require explicit user migration via `chainapi migrate`
- Minor version bumps are backward-compatible additions
- The app supports the **last 3 major versions** simultaneously
- Deprecated fields emit warnings for one minor version cycle before removal
- A schema linter (`chainapi lint`) catches deprecated patterns before they become errors

### 5.10 Hooks & Scripting API

Most signing and dynamic-payload needs are covered by the built-in functions in §5.7 and the named auth strategies in §5.10.1. Hooks are the escape hatch for the genuine long tail (custom encryption, request-rewriting based on a previous response, vendor-specific signature schemes).

**Hook authoring rules**

- Hooks live in **sibling `.js` files**, referenced by relative path. Inline JS-in-YAML strings are supported but discouraged — they lose syntax highlighting, formatting, and editor LSP support.
- The project ships a generated `chainapi.d.ts` so any editor with TypeScript LSP gives autocomplete on `ctx.request`, `ctx.env`, `ctx.actors`, and the built-in helpers (`hmac`, `jwt`, `base64`, …).
- Hooks are sandboxed via QuickJS — no filesystem, no network beyond the request itself, no `require`. The built-in helpers from §5.7 are exposed on the `ctx` object so hooks don't need to re-implement crypto.
- 1-second timeout per hook. Hooks have read-only access to other actors' variables; can only write to their own request/response.
- Hooks are explicitly opt-in via the `pre_request` / `post_response` keys (no implicit script execution).
- Errors in hooks fail the operation with a clear message pointing to the script and line number.

```yaml
operations:
  create_signed:
    method: POST
    path: /api/v1/secure/payment
    actor: customer
    pre_request:  ./hooks/sign-payment.js      # path relative to this YAML file
    post_response: ./hooks/decrypt-response.js
```

```js
// hooks/sign-payment.js
// ctx.request is mutable; ctx.env / ctx.secret / ctx.actors are read-only.
// All §5.7 built-ins are exposed on ctx (ctx.hmac, ctx.jwt, ctx.base64, …).
export default function (ctx) {
  const canonical = ctx.json.stringify(ctx.request.body);
  ctx.request.headers['X-Signature'] =
    ctx.hmac.sha256(ctx.secret.HMAC_KEY, canonical);
}
```

Inline form is still legal for one-liners and migrations from Postman scripts:

```yaml
operations:
  create_signed:
    pre_request: |
      ctx.request.headers['X-Signature'] =
        ctx.hmac.sha256(ctx.secret.HMAC_KEY, ctx.json.stringify(ctx.request.body));
```

#### 5.10.1 Named Auth Strategies (Built-in)

Common signed-auth schemes are first-class strategies, not hooks. The MVP ships with:

| Strategy | Use For |
|---|---|
| `simple` | Single-shot username/password login (default, shown in §5.5) |
| `chain` | Multi-step auth (OTP, magic-link confirmation, MFA) — shown in §5.5 |
| `oauth2_client_credentials` | Service-to-service OAuth2 |
| `oauth2_password` | Resource-owner password grant |
| `oauth2_authorization_code` | Browser-based OAuth (with PKCE; opens system browser) |
| `oauth1` | OAuth 1.0a (Twitter v1, legacy APIs) |
| `aws_sigv4` | AWS API Gateway, Lambda, S3-compatible services |
| `api_key` | Static key in header / query / cookie |
| `basic` | HTTP Basic |

```yaml
# AWS SigV4 — no hook required
auth:
  strategy: aws_sigv4
  region:     ap-south-1
  service:    execute-api
  access_key: "{{secret.AWS_ACCESS_KEY}}"
  secret_key: "{{secret.AWS_SECRET_KEY}}"
  session_token: "{{secret.AWS_SESSION_TOKEN}}"   # optional, for STS

# OAuth2 client credentials
auth:
  strategy: oauth2_client_credentials
  token_url: "{{env.baseUrl}}/oauth/token"
  client_id:     "{{secret.OAUTH_CLIENT_ID}}"
  client_secret: "{{secret.OAUTH_CLIENT_SECRET}}"
  scope: "read:orders write:orders"
inject:
  headers:
    Authorization: "Bearer {{actor.access_token}}"
```

A built-in strategy that can't be expressed declaratively (vendor-specific signing, non-standard token exchange) falls back to the `chain` strategy with `pre_request` hooks on individual steps.

### 5.11 Polling & Async Operations

Many production APIs return `202 Accepted` for write operations and require the client to poll a status endpoint until completion. ChainAPI models polling as a first-class part of an operation, not a separate "wait" primitive.

```yaml
operations:
  pay:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/pay
    actor: customer
    body:
      method: "card"
      token:  "tok_test_visa"
    expect_status: [200, 202]                # 202 triggers the poll

    poll_until:
      method: GET
      path: "{{response.headers.Location}}"  # or an explicit /api/v1/payments/{{response.body.data.id}}/status
      actor: customer                        # defaults to the parent op's actor
      success_when: "$.status == 'COMPLETED'"
      fail_when:    "$.status in ['FAILED', 'CANCELLED', 'EXPIRED']"
      interval: 2s                           # fixed interval, or:
      # backoff: { base: 500ms, factor: 2, max: 10s, jitter: 0.2 }
      timeout: 60s
      max_attempts: 30

    extract:
      payment_id:    $.data.payment.id       # extracts run against the FINAL poll response
      payment_state: $.status
```

**Engine semantics**

- The initial request runs once. If its response status is in the `202`-equivalent set (or the user explicitly asks for polling regardless), the engine begins polling.
- `success_when` and `fail_when` are predicate expressions over the poll response (`$` is the response body; `$.status_code` is also available). If both match a single response, `fail_when` wins.
- A `fail_when` clause is **strongly recommended**. Without it, every terminal failure state turns into a `timeout` after `timeout` / `max_attempts` — wasted minutes on every test of a known-bad path.
- Each poll attempt is recorded as a step in the run timeline. The timeline shows the predicate evaluations so users can see exactly when and why polling stopped.
- Extractions on the parent operation run against the **final** poll response (whichever response satisfied `success_when`), not the initial `202`.
- `interval` accepts a duration (`500ms`, `2s`, `1m`); `backoff` accepts the standard exponential-with-jitter form. Use one or the other, not both.
- `timeout` is the wall-clock cap; `max_attempts` is the request-count cap; whichever fires first ends the poll.
- The polling step shares the parent operation's actor and inherits its session; override with an explicit `actor:` if the status endpoint requires a different context.

**Out of scope for MVP**

- Webhook-driven waits (`wait_for_webhook` against a local listener). These require a local HTTP receiver and a tunnel for cloud APIs; the audience can fake webhooks adequately with `poll_until` against a status endpoint. Tracked for post-MVP.
- WebSocket / SSE-driven completion. Tracked alongside FR-3.8.

---

## 6. Functional Requirements

### 6.1 Schema & Project Management

- **FR-1.1** Load a project from a folder containing `chainapi.yaml` + sub-files
- **FR-1.2** Validate schema on load; show clear errors with file + line numbers
- **FR-1.3** Hot-reload on file changes (file watcher)
- **FR-1.4** Support multiple projects open in tabs
- **FR-1.5** Export project as a single bundled YAML for sharing
- **FR-1.6** Schema linter (`chainapi lint`) flags broken references, circular deps, deprecated fields

### 6.2 Execution Engine

- **FR-2.1** Execute a single operation by ID, auto-resolving its dependency chain
- **FR-2.2** Support sequential execution of an entire flow (multiple operations)
- **FR-2.3** Cache actor sessions with TTL; auto-refresh on expiry
- **FR-2.4** Cache resource extractions for the run lifetime; manual reset available
- **FR-2.5** Retry failed prerequisite calls (configurable per operation)
- **FR-2.6** Stop execution on first failure; show full execution log
- **FR-2.7** Support pre-request and post-response hooks (sandboxed JS) for edge cases
- **FR-2.8** "Dry run" mode — show resolved chain + final request body without executing
- **FR-2.9** Support polling operations (`poll_until`) per §5.11 — predicate-driven success/fail, configurable interval or exponential backoff, wall-clock and attempt-count caps, every poll attempt visible in the timeline
- **FR-2.10** Support named auth strategies (§5.10.1) including `oauth2_client_credentials`, `oauth2_authorization_code` (with PKCE), `aws_sigv4`, `oauth1`, `api_key`, `basic` — no hook required for any of these

### 6.3 HTTP Client

- **FR-3.1** Support all HTTP methods (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS)
- **FR-3.2** Support request body types: JSON, form-data, x-www-form-urlencoded, raw text, binary
- **FR-3.3** Support file uploads with size limits
- **FR-3.4** Support custom headers and query parameters
- **FR-3.5** Support TLS verification toggle (for self-signed dev certs)
- **FR-3.6** Support proxy configuration
- **FR-3.7** Configurable timeouts per operation
- **FR-3.8** WebSocket and SSE support (post-MVP)

### 6.4 Variable Extraction

- **FR-4.1** JSONPath extraction from response body
- **FR-4.2** XPath extraction (for XML APIs)
- **FR-4.3** Header extraction (for tokens in `Authorization` headers)
- **FR-4.4** Status code capture
- **FR-4.5** Regex extraction (fallback for non-structured responses)
- **FR-4.6** Cookie extraction (for cookie-based auth)

### 6.5 UI — Project Explorer

- **FR-5.1** Tree view of actors (with auth status indicator) and resources (with operations)
- **FR-5.2** Search/filter operations by name, path, method
- **FR-5.3** Right-click context menu: Run, Run with Override, Edit Schema, View Dependencies
- **FR-5.4** Visual indicator of cache state (which sessions valid, which resources cached)

### 6.6 UI — Request Editor

- **FR-6.1** Read-only by default; "Override Mode" allows one-shot modifications
- **FR-6.2** Show resolved request (after variable substitution) before execution
- **FR-6.3** Show dependency chain that will execute (preview)
- **FR-6.4** "Send" button executes; "Send Cleanly" resets caches first

### 6.7 UI — Response Viewer

- **FR-7.1** JSON tree view with collapsible nodes
- **FR-7.2** Raw view with syntax highlighting
- **FR-7.3** Headers tab
- **FR-7.4** Click any field in JSON → copy as JSONPath (for adding to extractions)
- **FR-7.5** Diff view between current and previous response
- **FR-7.6** Timeline view of the full executed chain (each step's request/response)

### 6.8 UI — Dependency Graph (Phase 2)

- **FR-8.1** Visual DAG of all resources and their dependencies
- **FR-8.2** Click a node → see operation details
- **FR-8.3** Highlight execution path when running
- **FR-8.4** Detect circular dependencies and warn

### 6.9 AI Doc Importer

- **FR-9.1** Accept input formats: OpenAPI 3.x (YAML/JSON), Markdown API docs, curl command lists, raw HAR files
- **FR-9.2** Send content to LLM (user's API key — OpenAI, Anthropic, or local Ollama) with structured prompt
- **FR-9.3** Generate ChainAPI YAML schema with inferred actors, resources, and dependencies
- **FR-9.4** Show diff/preview before writing files
- **FR-9.5** Allow user corrections; learn from corrections within session
- **FR-9.6** Store no data in the cloud; all LLM calls go from user's machine to their chosen provider
- **FR-9.7** Run a **verification pass** against sample responses (from OpenAPI examples / Postman responses / HAR / synthetic) for every proposed extraction. Refuse to write any operation where extractions don't resolve cleanly. (See §10.3.1.)
- **FR-9.8** Tag every AI-generated operation with a `_provenance` block recording source, model, timestamp, the verification source used, and the per-field evidence string. (See §10.3.3.)
- **FR-9.9** Surface a per-field evidence string for every AI inference in the review UI — not a confidence score. (See §10.3.2.)
- **FR-9.10** When an AI-imported operation fails at runtime, show targeted diagnostics that cross-reference the failure with the original inference and the actual response shape. (See §10.3.4.)
- **FR-9.11** Run timeline shows the value produced by every `extract` step, with `null` results highlighted and traced to their downstream consumers. (See §10.3.5.)

**AI Importer Acceptance Criteria** (must hit all to ship): see §10.5.

### 6.10 Postman / Bruno / Insomnia Migration

- **FR-10.1** Direct import of Postman collection JSON (v2.1+)
- **FR-10.2** Direct import of Bruno collections (folder of `.bru` files)
- **FR-10.3** Direct import of Insomnia v4 export
- **FR-10.4** Map Postman environments → ChainAPI environments
- **FR-10.5** Map Postman folder structure → resources (best-effort)
- **FR-10.6** Convert Postman pre-request/test scripts to ChainAPI hooks (best-effort with warnings)
- **FR-10.7** Show pre-import preview with confidence indicators per item

### 6.11 Environments & Secrets

- **FR-11.1** Switch environment via single dropdown
- **FR-11.2** Override environment variables per-run
- **FR-11.3** Store secrets in OS keychain (macOS Keychain, Windows Credential Manager, libsecret)
- **FR-11.4** Never write secrets to disk in plain text or to logs

### 6.12 History & Replay

- **FR-12.1** Auto-save every executed request/response to local DB
- **FR-12.2** Filter history by date, status, operation
- **FR-12.3** Replay any historical request with current variables
- **FR-12.4** Compare two historical runs of the same operation
- **FR-12.5** Configurable retention (default 30 days, max 365)

### 6.13 CLI Mode

- **FR-13.1** `chainapi run <operation>` executes from terminal using same schema
- **FR-13.2** `chainapi run --flow=full-onboarding` runs a named flow
- **FR-13.3** Output formats: text (human), JSON (machine), JUnit XML (CI)
- **FR-13.4** Exit codes for CI integration (0 = pass, non-zero = fail)
- **FR-13.5** Headless mode for CI/CD pipelines
- **FR-13.6** `chainapi lint` validates schema, returns non-zero on errors

### 6.14 Command Palette

- **FR-14.1** Cmd/Ctrl+P opens fuzzy-find palette over all operations
- **FR-14.2** Palette supports actions (Run, Run Cleanly, Edit, View Dependencies)
- **FR-14.3** Recent operations appear at top
- **FR-14.4** Supports `>` prefix for global commands (Switch Environment, Reset Cache, etc.)

---

## 7. Non-Functional Requirements

### 7.1 Performance

| Scenario | Target |
|----------|--------|
| Cold start (app launch to interactive) | < 2 seconds |
| Schema parse + validate (500 operations) | < 500ms |
| Render request panel after click | < 100ms |
| Single HTTP request overhead (ChainAPI vs. raw curl) | < 50ms |
| Memory footprint (idle) | < 250 MB |
| Memory footprint (heavy use, 100 requests in history) | < 500 MB |

### 7.2 Reliability

- **NFR-2.1** No data loss on crash — all schema edits saved to disk; runs persisted to local DB
- **NFR-2.2** Graceful handling of network failures with clear error messages
- **NFR-2.3** Recoverable from corrupt state — "reset cache" button always works
- **NFR-2.4** Crash rate < 0.1% of sessions (Sentry-style reporting, opt-in)

### 7.3 Privacy & Security

- **NFR-3.1** Local-first — no telemetry without explicit opt-in
- **NFR-3.2** Secrets never leave the user's machine
- **NFR-3.3** AI imports use the user's own LLM API key — no proxy through ChainAPI servers
- **NFR-3.4** TLS-only for outbound requests by default (user can override per-environment)
- **NFR-3.5** Code signing on all binaries (Apple notarization, Windows Authenticode)
- **NFR-3.6** Hooks sandboxed (QuickJS, no filesystem/network access)

### 7.4 Cross-Platform

- **NFR-4.1** macOS 12+ (Apple Silicon + Intel)
- **NFR-4.2** Windows 10/11 (x64, ARM64)
- **NFR-4.3** Linux (Ubuntu 22+, Fedora 38+, Arch — `.deb`, `.rpm`, AppImage)

### 7.5 Accessibility

- **NFR-5.1** Full keyboard navigation; no mouse-only flows
- **NFR-5.2** Screen reader compatible (semantic labels on all controls)
- **NFR-5.3** WCAG 2.1 AA contrast ratios in both light and dark themes
- **NFR-5.4** Respects OS font scaling

### 7.6 Updates & Distribution

- **NFR-6.1** Built-in auto-update (Sparkle on macOS, Squirrel on Windows, AppImage update on Linux)
- **NFR-6.2** Stable channel (default) and beta channel (opt-in)
- **NFR-6.3** Updates downloaded in background, applied on next launch (not forced)
- **NFR-6.4** Rollback to previous version possible from settings

---

## 8. System Architecture

### 8.1 High-Level Components

```
┌─────────────────────────────────────────────────────────────┐
│              Qt 6 Desktop App  (UI Shell, C++)              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   Project    │  │   Request    │  │   Response   │      │
│  │   Explorer   │  │    Editor    │  │    Viewer    │      │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│         │                 │                  │              │
│  ┌──────▼─────────────────▼──────────────────▼───────┐      │
│  │      Application State  (QObject signals/slots,    │      │
│  │              MVVM with Q_PROPERTY)                 │      │
│  └──────┬─────────────────┬──────────────────┬───────┘      │
│         │                 │                  │              │
└─────────┼─────────────────┼──────────────────┼──────────────┘
          │                 │                  │
   ┌──────▼─────┐    ┌──────▼──────┐    ┌──────▼──────┐
   │  Engine Façade  (in-process C++ today; replaceable │
   │  with out-of-process IPC binary in Phase 5+)       │
   └──────┬──────────────────┬──────────────────┬──────┘
          │                  │                  │
   ┌──────▼──────┐    ┌──────▼──────┐    ┌──────▼──────┐
   │   Schema    │    │  Execution  │    │   History   │
   │  (yaml-cpp) │    │   Engine    │    │  (SQLite)   │
   └──────┬──────┘    └──────┬──────┘    └──────┬──────┘
          │                  │                  │
   ┌──────▼─────┐    ┌──────▼──────┐    ┌──────▼──────┐
   │   YAML     │    │ HTTP (libcurl│    │   SQLite    │
   │   Files    │    │ /QNetwork) + │    │  (embedded) │
   │            │    │ DAG Resolver │    │             │
   └────────────┘    └─────────────┘    └─────────────┘

  Auxiliary embedded subsystems (linked as native C/C++ libs):
  • QScintilla — code editor surface (YAML, JSON, JS hooks)
  • QuickJS    — sandboxed JS for pre/post hooks
  • QtKeychain — OS keychain access
```

### 8.2 Layers (Clean Architecture)

| Layer | Responsibility | Key Classes |
|-------|----------------|-------------|
| **Presentation** | Qt widgets, screens (QWidget / Qt Widgets, MVVM via QObject) | `ProjectExplorerWidget`, `RequestEditorPanel`, `ResponseViewerPanel` |
| **Application** | Use cases, state coordination, view models | `RunOperationUseCase`, `ImportFromOpenApiUseCase`, view-model `QObject`s with `Q_PROPERTY` |
| **Domain** | Entities, value objects, domain services. Pure C++ (no Qt types) | `Project`, `Actor`, `Resource`, `Operation`, `DependencyResolver`, `RunContext` |
| **Infrastructure** | YAML parsing, HTTP client, SQLite, keychain, JS sandbox | `YamlSchemaRepository` (yaml-cpp), `HttpClient` (libcurl or QNetworkAccessManager), `SqliteHistoryStore`, `QtKeychainSecretStore`, `QuickJsHookRunner` |

**Critical architectural rule (preserves the future Option B path):** the **Domain** and **Infrastructure** layers MUST NOT depend on any Qt header outside of `QtCore` types like `QString` and `QByteArray` — and even those are confined behind thin adapters. The dependency-resolution engine, schema parser, HTTP client, cache, and history store are buildable as a pure C++ library (`libchainapi-engine`) that links into the Qt UI today and into a separate process or other language binding tomorrow.

### 8.3 Execution Engine Internals

```cpp
// Pure-C++ engine class — no Qt UI dependency.
// Lives in libchainapi-engine; today linked into the Qt app,
// tomorrow extractable to a CLI binary or out-of-process daemon.
class ExecutionEngine {
public:
  RunResult run(OperationId target, RunContext& ctx);

private:
  DependencyResolver resolver_;
  StepExecutor      executor_;
};

RunResult ExecutionEngine::run(OperationId target, RunContext& ctx) {
  Chain chain = resolver_.resolve(target);
  for (const Step& step : chain) {
    StepResult result = executor_.execute(step, ctx);
    ctx.record(step, result);
    if (result.failed())
      return RunResult::failed(chain, ctx);
  }
  return RunResult::succeeded(chain, ctx);
}
```

### 8.4 Engine Error Handling Strategy

| Failure Mode | Engine Behavior |
|-------------|-----------------|
| Network timeout | Configured retry, then fail step, halt chain, surface in UI |
| 401 on actor session | Auto-refresh once; if still 401, fail with "session refresh failed" |
| 5xx on dependency | Halt chain, mark target as "blocked", show which dep failed |
| 4xx on dependency | Halt chain (assumes test data issue); show response body |
| Variable missing | Halt before send; show "missing: {{X}}, last set by: never" |
| Hook script error | Treat as operation failure; show stack trace in console panel |
| Malformed JSON response | Capture as text, fail extraction, allow user to inspect raw |
| Circular dependency detected | Halt at schema load; refuse to run any operation |

### 8.5 Tech Stack

| Concern | Choice | Why |
|---------|--------|-----|
| **App framework** | **Qt 6** (Widgets, with QML for graph viz only) | Native widgets per OS; lowest memory + battery footprint; mature for IDE-class apps |
| **Language** | **C++17/20** | Native, lean, no managed runtime; aligns with Qt-native ecosystem |
| **Build system** | **CMake** | Standard for Qt 6; reproducible, cross-platform |
| **State / data binding** | **QObject signals/slots + Q_PROPERTY (MVVM)** | Built-in, native; no third-party state library needed |
| **HTTP client** | **libcurl** (preferred) or **QNetworkAccessManager** | libcurl is decoupled from Qt — keeps engine portable; QNetwork is fine for prototype |
| **YAML parsing** | **yaml-cpp** | Mature, YAML 1.2 compliant, header-light |
| **JSON** | **nlohmann/json** or built-in `QJsonDocument` | nlohmann decouples from Qt for engine; `QJsonDocument` for UI-side conveniences |
| **Storage** | **SQLite** (via `sqlite3` C API or **SQLiteCpp**) | Embedded, single-file, durable; engine remains Qt-free |
| **Secret storage** | **QtKeychain** | Cross-platform wrapper over Keychain / Credential Manager / libsecret |
| **Code editor** | **QScintilla** | Native, decades of battle-testing, supports YAML / JSON / JS |
| **JS sandbox (hooks)** | **QuickJS** (linked as a C library) | Sandboxed, no FS/network, ~600 KB; engine-side, not Qt-side |
| **Graph viz** | **Qt Quick / QML** + custom canvas | Embedded into the Widgets app via `QQuickWidget` for the dependency-graph view only |
| **HTTP/JSONPath in engine** | **JSONPath via nlohmann/json + custom evaluator** or **JsonCons** | Engine remains language-agnostic |
| **LLM client** | **libcurl + nlohmann/json** | Plain HTTPS to user-chosen LLM endpoint; no SDK needed |
| **Auto-update** | **Sparkle (macOS)**, **WinSparkle (Windows)**, **AppImageUpdate (Linux)** | Native, well-tested mechanisms |
| **Code signing** | Apple notarization, Windows Authenticode, Linux signed AppImage | Per NFR-3.5 |
| **CI** | GitHub Actions matrix (macOS/Win/Linux × Debug/Release) | Standard |

### 8.6 Two-Phase Architecture (Option A → Option B path)

To stay disciplined and keep the future option open without paying its cost today:

**Phase A (MVP, locked in):** UI shell and engine ship as a **single Qt application** with the engine compiled in-process as a static or shared library (`libchainapi-engine`). Fastest path to a shipped product. The engine has zero Qt-UI dependencies — only `QtCore` value types behind narrow adapter interfaces.

**Phase B (post-MVP, kept open):** when the CLI (FR-13.x) demands a serious second consumer of the engine, or when team workspaces / cloud (Phase 8) demand a server-side runner, the engine is extracted into a **separate process** (or even a separate language — Rust is the leading candidate for memory-safety + no-GC). Communication moves to an IPC contract (JSON-RPC over stdio is the working assumption).

**Architectural guardrails enforced from day one to make Phase B cheap:**

| Guardrail | Mechanism | Verified by |
|-----------|-----------|-------------|
| Engine has no Qt-UI types | Module split: `libchainapi-engine` does not link `Qt::Widgets` / `Qt::Gui` / `Qt::Quick` | CMake target dependency check in CI |
| Engine has no UI threading assumptions | All engine APIs return values or invoke callbacks; no Qt signals from engine | Code review checklist |
| Engine surface is callable as a clean C++ API | All public engine APIs use STL types or POD `QtCore` types only | Header review |
| Engine's serialization formats are language-agnostic | Schema is YAML; persisted history is SQLite; events are POD structs serializable to JSON | Existing decisions |
| Hooks sandbox is process-local but not UI-bound | QuickJS lives in the engine library, not the UI | Module structure |
| No global singletons that bind the engine to a specific process model | Engine is a constructable `ExecutionEngine` object; CLI/desktop construct their own instance | Code review |

If those rules hold throughout the MVP, splitting the engine into a separate process becomes a build-system change (and an IPC layer), not a rewrite.

---

## 9. UI/UX Design Principles

### 9.1 Layout (Default)

```
┌─────────────────────────────────────────────────────────────┐
│  [Project ▼]  [Env: local ▼]   ⚙ ❓ 👤        🌙           │
├─────────────────────────────────────────────────────────────┤
│ Explorer       │ Request                  │ Response        │
│                │                          │                 │
│ ▼ Actors       │ POST /api/v1/orders      │ 201 Created     │
│   ✓ admin      │ Actor: customer          │ 245ms           │
│   ✓ vendor     │                          │                 │
│   ⊘ customer   │ Will execute:            │ {               │
│                │   1. customer.login      │   "data": {     │
│ ▼ Resources    │   2. product.create      │     "id": "...",│
│   ▶ product    │   3. product.publish     │     ...         │
│     • create   │   4. order.create (this) │   }             │
│     • publish  │                          │ }               │
│   ▶ order      │ Body:                    │                 │
│     • create   │ {                        │ Extracted:      │
│     • pay      │   "product_id": "...",   │ • order_id = ...│
│   ▶ refund     │   ...                    │                 │
│                │ }                        │                 │
│                │ [Send] [Send Cleanly]    │ [Diff] [History]│
└─────────────────────────────────────────────────────────────┘
```

### 9.2 Visual Design

- **Material 3** with seeded color scheme (deep blue or violet primary)
- **Dark mode default** (developers prefer it; respect OS setting)
- **Monospace font** for all code/JSON (JetBrains Mono or Fira Code)
- **Sans-serif UI font** for chrome (Inter or system)
- **Status colors**: green (200/201), yellow (3xx/4xx), red (5xx), grey (skipped)

### 9.3 Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Cmd/Ctrl+P | Quick-open operation (command palette) |
| Cmd/Ctrl+Enter | Run selected operation |
| Cmd/Ctrl+Shift+Enter | Run cleanly (reset caches first) |
| Cmd/Ctrl+R | Re-run last operation |
| Cmd/Ctrl+E | Switch environment |
| Cmd/Ctrl+, | Open settings |
| Cmd/Ctrl+Shift+R | Reset all caches |
| Cmd/Ctrl+/ | Toggle inline help |
| Cmd/Ctrl+B | Toggle explorer sidebar |
| Cmd/Ctrl+J | Toggle response panel |
| Cmd/Ctrl+1/2/3 | Switch tabs (Request, Response, Timeline) |
| Esc | Cancel running operation |
| F1 | Show help |

### 9.4 Empty States

- **No project loaded** → big call-to-action: "Open Project" / "Import from OpenAPI" / "Import from Postman" / "Try Sample Project"
- **No history yet** → "Run your first request to see history here"
- **AI import failed** → "Couldn't parse this format. Want to send it as plain text?"

---

## 10. AI Importer — Detailed Spec

### 10.1 Input Formats Supported

| Format | Detection | Handling |
|--------|-----------|----------|
| **OpenAPI 3.x** | YAML/JSON with `openapi:` key | Direct parse → infer actors from `securitySchemes`, resources from `paths` |
| **Postman Collection** | JSON with `info.schema` containing postman | Direct parse + LLM enrichment for dependencies |
| **Markdown API docs** | `.md` files with HTTP request examples | LLM-driven extraction |
| **curl commands** | Lines starting with `curl` | Parse + LLM enrichment |
| **HAR file** | JSON with `log.entries` | Direct parse + LLM grouping |
| **Free-form description** | Pasted text | LLM-driven full inference |

### 10.2 LLM Prompt Strategy

The prompt sends the LLM:
1. ChainAPI schema spec (so it knows the output format)
2. The user's input documents
3. Hints (e.g., "this API has admin/vendor/customer roles")
4. Few-shot examples of correct schema generation

The LLM returns:
- Proposed actors with auth chains
- Proposed resources with operations
- Proposed dependencies between operations
- Confidence score per inference

### 10.3 Verify-Before-Write & Review Flow

A 90%-accurate importer is more dangerous than a 60%-accurate one because users *trust it*. They accept the YAML, hit run, get a `404`, and have no debugging anchor. The flow is designed around four behaviors that turn silent wrongness into visible wrongness.

#### 10.3.1 Verification Pass (Mandatory, before any file is written)

After the LLM proposes a schema, the importer runs a probe pass against representative sample responses:

1. For each operation, obtain a sample response — preferred sources, in order:
   - OpenAPI `examples` / response schemas (when present)
   - Postman / Insomnia / Bruno `response` blocks captured alongside requests
   - HAR file response bodies
   - As a last resort, ask the LLM for a synthetic sample matching the documented schema
2. Evaluate every proposed `extract` JSONPath against the sample response.
3. Tag each extraction:
   - ✅ **verified** — JSONPath matched a non-null scalar of plausible type
   - ⚠️ **null** — JSONPath was structurally valid but produced no value
   - ❌ **no match** — JSONPath did not resolve at all
4. **The importer refuses to write any operation whose extractions are not all ✅.** Failed extractions are surfaced in the review UI as "needs your input" rather than written silently.

This single rule eliminates the entire class of "AI generated YAML, run gives 404, user has no idea why" failures.

#### 10.3.2 Show Evidence, Not Confidence Scores

A bare `confidence: 0.85` is unactionable. Each AI-inferred field carries a short evidence string visible on hover or in a dedicated review pane:

> *Inferred `actor: vendor` because the OpenAPI security scheme `BearerAuth` is used on this endpoint, and `vendor` is the only actor that obtains a Bearer token (from `/api/v1/auth/vendor/login`).*

> *Inferred dependency `order.create` for `order.pay` because the path template `/orders/{order_id}/pay` references `{order_id}`, which is the documented response field of `POST /orders`.*

Users can spot wrong reasoning. They cannot spot wrong probabilities.

#### 10.3.3 Provenance Tagging

Every AI-imported entity is tagged with `_provenance`:

```yaml
operations:
  create:
    method: POST
    path: /api/v1/products
    extract:
      product_id: $.data.id
    _provenance:
      source: ai_import
      model:  gpt-4o
      imported_at: 2026-05-24T10:30:00Z
      verified_against: openapi_example   # or postman_response, har, synthetic
      evidence:
        actor: "...inferred from BearerAuth scheme..."
        extract.product_id: "verified against POST /products `examples.default`"
```

Provenance is metadata-only and stripped from the engine's runtime view; tooling and UX consume it to drive runtime diagnostics (§10.3.4) and to generate "AI Import Audit" reports.

#### 10.3.4 Targeted Runtime Diagnostics for AI-Imported Operations

When an operation tagged with `_provenance.source: ai_import` fails at runtime, the response panel shows AI-aware diagnostics that cross-reference the failure with the importer's inferences:

> **`order.pay` returned 404. This operation was AI-imported.**
>
> Likely cause: the path template references `{{order.order_id}}`, which resolved to `null`. The extraction `order_id: $.data.id` on `order.create` produced no match against the actual response.
>
> The actual response from `order.create` was:
> ```json
> { "data": { "order": { "id": "ord_abc123" } } }
> ```
> The AI inferred `$.data.id`. Try `$.data.order.id`. [Apply Fix] [Edit Schema]

To make this possible, the engine must:
- Track which extractions resolved to `null` per run, not just non-null ones.
- Link a failing variable reference (`{{order.order_id}}`) back to the operation that should have produced it.
- Walk back through dependencies to find the most recent failed extraction on the chain.

#### 10.3.5 Visible Data Flow in the Timeline

In the run timeline panel, each step displays a one-line strip showing what its `extract` block produced. Resolved values render in green; `null` results render in red with an arrow to the next operation that consumes them.

Most "subtly wrong extraction" bugs become obvious the moment users *see* the data instead of reading YAML. This view is independent of AI import — it benefits hand-written schemas too — but it's the primary debugging surface for hallucinated extractions.

#### 10.3.6 Review UI Flow

1. Import → LLM proposes schema → verification pass runs.
2. Side-by-side review:
   - Left: input docs (highlighted to show what each operation derives from)
   - Right: proposed YAML, with per-field evidence on hover and ✅/⚠️/❌ tags on every extraction
3. User can:
   - Accept all (only enabled when all extractions are ✅)
   - Edit YAML inline with live re-verification
   - Reject specific operations
   - Re-prompt with corrections ("the customer also needs phone OTP, not email login")
   - Skip to manual completion for ⚠️/❌ items
4. Final schema written with `_provenance` tags intact.

### 10.4 LLM Provider Support

- OpenAI (GPT-4o, GPT-4 Turbo)
- Anthropic (Claude Sonnet, Claude Opus)
- Local Ollama (for privacy-conscious users)
- BYO API key — stored in keychain, never sent to ChainAPI servers

### 10.5 Acceptance Criteria

The MVP ships only when all of the following hold against a curated benchmark of 20 real-world API specs (mix of OpenAPI, Postman, Markdown, HAR; both small <50-endpoint and large >200-endpoint inputs):

- **Operation extraction**: ≥ 90% of operations correctly inferred from OpenAPI input; ≥ 70% from Markdown / curl / free-form
- **Auth flow inference**: ≥ 90% for OpenAPI; ≥ 75% for Postman collections with non-trivial auth
- **Dependency inference**: ≥ 60% of inferable dependencies correctly identified
- **Verification pass coverage**: 100% of written extractions are ✅ verified — never ⚠️ or ❌. Operations with unresolvable extractions are surfaced for manual completion, never silently written.
- **Time-to-first-run**: ≤ 5 minutes from "paste docs" to first successful run on a reasonable input
- **AI import cost**: typically < $0.05 per import using GPT-4o-mini or Claude Haiku for the proposal pass; verification pass adds < $0.02
- **Provenance integrity**: every AI-imported operation carries a non-empty `_provenance` block; runtime diagnostics correctly identify the failing AI inference for at least 80% of seeded extraction-error scenarios in the test harness

---

## 11. Migration from Postman / Bruno / Insomnia

This is **critical for adoption** — no developer will switch tools without a one-click import path.

### 11.1 Postman Collection Import

- Accept Postman Collection JSON (v2.1+)
- Map Postman folders → resources (best-effort, with manual override)
- Map Postman environments → ChainAPI environments
- Map Postman pre-request/test scripts → ChainAPI hooks (with warnings for unsupported APIs like `pm.collectionVariables`)
- Detect token extraction patterns in scripts → convert to declarative `extract` blocks
- Show confidence score per imported item; let user review before commit

### 11.2 Bruno Import

- Accept folder of `.bru` files
- Bruno's structure already maps cleanly to actors/resources
- Convert Bruno JS scripts to ChainAPI hooks

### 11.3 Insomnia Import

- Accept Insomnia v4 export (JSON)
- Map Insomnia request groups → resources
- Map Insomnia environments → ChainAPI environments

### 11.4 Migration Quality Bar

- Postman collections of < 50 requests should import with ≥ 80% success (operations runnable as-is)
- All authentication patterns (Bearer, API key, Basic, OAuth2) supported out of the box
- A user should be able to switch from Postman to ChainAPI in < 30 minutes for a typical project

---

## 12. First-Run Experience

User opens ChainAPI for the first time. What they see:

**Step 1 — Welcome screen** (3 seconds)
- "ChainAPI: API testing without the copy-paste"
- 3 buttons: **Try Sample Project**, **Open Existing Project**, **Import**

**Step 2a — If "Try Sample"**
- Loads bundled MarketplaceAPI sample (with mock server running locally)
- Highlights "click any endpoint" tooltip
- User clicks `refund.approve` → sees the chain run → sees response
- "You just ran 9 chained requests. Here's how it works…" inline tutorial (skippable)

**Step 2b — If "Import"**
- Choose source: OpenAPI / Postman / Bruno / Insomnia / Markdown / Other
- File picker or paste box
- For non-OpenAPI sources, prompt for LLM key (with "skip — manual mode" option)
- Show preview, accept, write project

**Step 2c — If "Open Project"**
- Standard folder picker
- Validate schema, show errors clearly if any

**Step 3 — Project loaded**
- Explorer populated
- First operation pre-selected
- Subtle "press Cmd+Enter to run" hint

**Success metric**: 80% of users complete a successful first run within 10 minutes of installing.

---

## 13. Phased Roadmap

### 13.1 Phase 0 — Validation (Week 1)

- [ ] Write the schema spec and validate against 3 real-world APIs (one OSS sample + two private)
- [ ] Hand-author YAML for a sample MarketplaceAPI covering 20 endpoints
- [ ] Confirm schema can express all needed flows
- [ ] Recruit 5–10 backend devs for design partner interviews

### 13.2 Phase 1 — Engine Only, CLI (Weeks 2–3)

- [ ] Schema parser + validator + linter
- [ ] Dependency resolver (DAG)
- [ ] HTTP execution
- [ ] Variable extraction (JSONPath)
- [ ] Session caching with TTL
- [ ] CLI: `chainapi run <op>` with text + JSON output
- [ ] **Milestone**: Run any sample endpoint from terminal in one command

### 13.3 Phase 2 — Qt Desktop UI (Weeks 4–6)

- [ ] Project explorer (tree view)
- [ ] Request editor (read-only + override mode)
- [ ] Response viewer (JSON tree, raw, headers)
- [ ] Execution timeline (chain visualization)
- [ ] Environment switcher
- [ ] Secret management (keychain)
- [ ] History (SQLite + replay)
- [ ] Command palette
- [ ] **Milestone**: Full GUI usable for daily testing

### 13.4 Phase 3 — Migration & AI Importer (Weeks 7–8)

- [ ] Postman collection import (direct, no LLM)
- [ ] Bruno + Insomnia import
- [ ] OpenAPI direct parser (no LLM)
- [ ] LLM-driven Markdown/curl import
- [ ] Review + edit UI
- [ ] BYO API key flow
- [ ] **Milestone**: Import any well-documented API in under 5 minutes; Postman collection in < 30 min

### 13.5 Phase 4 — Polish & v1 Release (Weeks 9–10)

- [ ] Dependency graph visual
- [ ] Diff view for responses
- [ ] Pre/post hooks (JS sandbox)
- [ ] Auto-update
- [ ] Documentation site
- [ ] Sample projects (3+ — marketplace, school SaaS, project management)
- [ ] Public launch (GitHub, ProductHunt, HN)

### 13.6 Post-MVP

- **Phase 5** — Browser extension (capture requests from running apps to auto-build schema)
- **Phase 6** — Team sync via git (lockfiles for shared state)
- **Phase 7** — **Mock server mode (G7)** — schema-driven mock server reusing `chainapi.yaml`:
  - "Serve" command launches a local HTTP server on a configurable port
  - Operations resolve to recorded responses from history, or to templated responses defined inline
  - Stateful mocks (the mock can update its own state when a write operation is hit, so chained flows work)
  - Useful for: frontend dev without backend, contract testing, demoing offline, reproducing rare error responses
- **Phase 8** — Hosted cloud version with team workspaces
- **Phase 9** — VSCode extension
- **Phase 10** — **Schema-driven API documentation (G8)** — generate a browsable docs site from `chainapi.yaml`:
  - Static HTML/Markdown output, themeable
  - Auto-includes real request/response examples captured during runs
  - Documents actor auth flows, resource dependencies, and operation contracts
  - Optional "Try it" button that delegates to the user's local ChainAPI instance via deep link
- **Phase 11** — WebSocket / SSE / GraphQL subscription support

---

## 14. Success Metrics

### 14.1 Product Metrics (3 months post-launch)

| Metric | Target |
|--------|--------|
| Active projects (loaded by users in last 7 days) | 1,000+ |
| Median time to test a chained endpoint | < 5 seconds (vs. 5–10 min in Postman today) |
| Median time from "first launch" to "first successful run" | < 10 minutes |
| AI importer success rate (user accepts ≥ 80% of generated schema) | 70%+ |
| Daily active users | 500+ |
| Postman migrations completed | 100+ |

### 14.2 Technical Metrics

| Metric | Target |
|--------|--------|
| App crash rate | < 0.1% sessions |
| Schema validation accuracy | 100% (no false positives) |
| Average request execution overhead vs. raw curl | < 50ms |

### 14.3 Business Metrics (6 months post-launch, if monetizing)

| Metric | Target |
|--------|--------|
| Free → Paid conversion | 3–5% |
| MRR | $5K+ |
| NPS | > 40 |

---

## 15. Pricing & Business Model

Three options considered:

### Option A — Pure Open Source (MIT/Apache)
- **Pros**: Maximum adoption, community contributions, no friction
- **Cons**: No revenue, no funding for sustained development
- **Verdict**: ❌ Unsustainable solo

### Option B — Open Core (Recommended)
- **Free (open source)**: Engine, schema spec, GUI, CLI, all core features, OpenAPI/Postman import
- **Paid (closed)**: AI importer, team workspace cloud sync, advanced analytics, SSO, audit logs
- **Pricing**: Free tier; Pro $12/user/month; Team $49/seat/month (with 5+ seats)
- **Pros**: Free version is genuinely useful (drives adoption); paid features target professional teams
- **Cons**: Need clear free/paid line; risk of bait-and-switch perception
- **Verdict**: ✅ Recommended for v1

### Option C — Closed Source SaaS
- **Pros**: Higher revenue per user, easier to monetize
- **Cons**: Devs prefer open-source tools; harder adoption against open competitors
- **Verdict**: ❌ Wrong fit for this audience

**Recommendation**: Open Core (Option B). Apache 2.0 license for the open part. AI importer becomes a paid feature in Phase 4 with a generous free trial (50 imports/month).

---

## 16. Validation Plan

Before writing production code, validate the concept:

### 16.1 Phase 0 Validation Activities

1. **Schema dogfooding** — Hand-write the schema for 3 real APIs (one personal project, two from design partners). Find what the schema can't express. Iterate.
2. **Design partner interviews** — Recruit 5–10 backend devs / QAs. Show the schema, the flow concept, mockups. Get reactions.
3. **Competitive deep-dive** — Use Postman Flows, Bruno, Stepci, Apidog hands-on for 1 week each. Document every friction point.
4. **AI importer feasibility** — Without writing code: paste 3 sample API docs into Claude/GPT-4 with the schema spec. Measure quality of output. If LLM accuracy is < 60% on bare prompts, the importer needs substantial engineering.

### 16.2 Go / No-Go Decision Gate

Build the MVP only if:
- Schema can express all 3 sample APIs without escape hatches (including at least one with a polling/async endpoint and one with HMAC or AWS SigV4 signing)
- ≥ 6 of 10 design partners say "I would switch to this from Postman"
- LLM accuracy for OpenAPI input is ≥ 80% with simple prompting **and** the verification pass (§10.3.1) successfully filters the remaining bad inferences without rejecting more than 5% of correct ones

If any fail, iterate or pivot before committing 10 weeks.

---

## 17. Competitive Analysis

| Tool | Strengths | Weaknesses | ChainAPI Differentiation |
|------|-----------|------------|--------------------------|
| **Postman** | Massive ecosystem, team features, mock servers | Manual chaining, bloated, slow | Auto-resolved dependencies, fast, focused |
| **Postman Flows** | Visual workflows | Manual wiring per relationship, premium feature | Schema-driven, no manual wiring |
| **Bruno** | Open source, git-friendly, lightweight | Manual scripting like Postman | Schema + AI import |
| **Insomnia** | Clean UI | No actor concept, manual chaining | First-class actors |
| **Hurl** | Plain text, CI-friendly | Linear scripts only, no GUI | Graph-based, GUI + CLI |
| **Stepci** | YAML workflows | Manual workflow definition | Auto-resolution from declarative schema |
| **Apidog** | Better Postman | Still manual workflows | AI import + dependency resolution |

### 17.1 Moat

- **Schema format ownership** — if devs adopt the schema, switching costs become high. Open-source it under Apache 2.0 to encourage adoption while keeping AI importer + cloud features paid.
- **AI importer quality** — staying ahead on prompt engineering and integration depth.
- **Migration paths** — best-in-class import from competitors lowers the switching cost from competitors but raises it from us (hard to reverse).
- **Speed** — being meaningfully faster than Postman is a real edge for a developer tool.

---

## 18. Risks & Mitigations

| Risk | Likelihood | Impact | Owner | Mitigation |
|------|------------|--------|-------|------------|
| **Schema feels too rigid; can't express edge-case APIs** | Medium | High | Tech lead | Add escape hatches (hooks); validate via Phase 0 against real APIs |
| **AI importer produces bad schemas** | Medium | High | AI lead | Mandatory verification pass (§10.3.1) refuses to write unverified extractions; per-field evidence (§10.3.2) makes wrong reasoning visible; provenance + runtime diagnostics (§10.3.3-4) give failed runs a debugging anchor; degrade to manual mode |
| **Postman ships native version of this** | Low | High | Founder | Stay focused, ship faster, open-source the engine to commoditize |
| **Adoption stalls — devs comfortable with Postman** | Medium | High | Marketing | Free tier with all core features; sample projects; viral demos; one-click Postman import |
| **Cross-platform Qt desktop bugs** | Medium | Medium | Tech lead | Test on all 3 OSes weekly; CI matrix builds (macOS / Windows / Linux × Debug / Release) |
| **LLM costs balloon for AI importer** | Low | Medium | Tech lead | BYO API keys; cache prompts; offer local Ollama option |
| **Securing secrets correctly across all OSes** | Medium | High | Security lead | Use battle-tested `QtKeychain`; security review before launch |
| **Hook sandbox escapes** | Low | High | Security lead | QuickJS isolation; explicit opt-in; no FS/network access; 1s timeout |
| **Schema spec needs breaking changes post-v1** | Medium | Medium | Tech lead | Versioned schema (`version: 1`); migration tool; support last 3 majors |
| **C++ memory-safety bugs (UAF, leaks)** | Medium | Medium | Tech lead | Use modern C++17/20 (smart pointers, RAII), Qt parent-child ownership, AddressSanitizer in CI Debug builds, leak detection on every PR |
| **C++ velocity slows MVP delivery** | Medium | Medium | Tech lead | Hard scope discipline; Phase 0 validation gates; reuse `libcurl`/`yaml-cpp`/`SQLiteCpp`/`QtKeychain`/`QScintilla` rather than building anything from scratch |

---

## 19. Open Questions

1. **Branding** — Is "ChainAPI" the right name? Domain availability + trademark check needed.
2. **Plugin system** — Should v1 ship a plugin API (custom auth schemes like AWS SigV4, HMAC), or is the JS hook escape hatch enough?
3. **Embeddable engine** — Should the engine be publishable as a Dart/Node library so test suites can use it directly (ChainAPI in Jest/Vitest tests)?
4. **Encrypted project files** — Some users will want to commit projects to private repos but encrypt sensitive parts. Is `.chainapi-vault` encrypted bundle worth shipping in v1?
5. **Real-time mock server** — Strong adjacent feature; could ride the same schema. Worth slipping into Phase 4?

---

## 20. Glossary

| Term | Definition |
|------|------------|
| **Actor** | An identity with its own auth flow (admin, vendor, customer) |
| **Resource** | A domain entity exposed via API endpoints (product, order) |
| **Operation** | A single endpoint action on a resource (create, get, publish) |
| **Chain** | The ordered sequence of operations executed to reach a target |
| **Run Context** | Runtime state holding active sessions and extracted variables |
| **Extraction** | Pulling a value from a response into a named variable |
| **Session** | A cached actor authentication (token + extracted user info) |
| **DAG** | Directed Acyclic Graph — the dependency structure between operations |
| **Hook** | Sandboxed JS pre/post script for edge-case logic |

---

## 21. Appendices

### Appendix A — Sample Schema for MarketplaceAPI

A complete sample schema covering 30 endpoints across 3 actors and 6 resources will live at `samples/marketplace/` in the repository. It will be loaded as the default "Try Sample Project" experience.

### Appendix B — Sample LLM Prompt for AI Importer

The system prompt template, few-shot examples, and post-processing logic will live at `prompts/import/` in the repository.

### Appendix C — ADRs

#### ADR-001 — Qt 6 + C++ for the desktop app

- **Status**: Accepted (PRD v0.3)
- **Context**: The product needs a native-feeling, low-memory, battery-efficient desktop tool with an IDE-class UI (project explorer, code editor, response viewer, dependency graph). Considered: Flutter desktop, Avalonia UI, Qt, Electron/Tauri.
- **Decision**: Qt 6 + C++17/20 + CMake. UI uses Qt Widgets primarily, with Qt Quick / QML embedded only for the dependency-graph view.
- **Rationale**:
  - Native widgets per OS — feels right to backend dev / QA audience
  - Lowest memory footprint and battery impact among options (no managed runtime, no Skia full-time renderer)
  - QScintilla provides a production-grade code editor for YAML / JSON / JS hooks
  - Mature ecosystem for everything we need: yaml-cpp, libcurl, SQLite, QtKeychain, QuickJS
  - Native auto-update via Sparkle / WinSparkle / AppImageUpdate
  - Strong long-term durability — Qt is 25+ years stable for desktop tooling
- **Trade-offs accepted**:
  - C++ has slower iteration than C# (Avalonia) or Dart (Flutter) — accepted in exchange for native lean output
  - Smaller community than Flutter — but Qt's community is the right kind (desktop-focused, IDE-builders)
  - Higher engineering rigor required (memory ownership, build complexity) — mitigated by modern C++17/20 and Qt's parent-child memory model
- **Alternatives rejected**:
  - **Flutter desktop** — custom-rendered UI is wrong for IDE-class tools; no first-class code editor; native menu integration on macOS is rough
  - **Avalonia UI** — strong second choice (AvaloniaEdit is excellent, .NET tooling is great), but loses on memory + battery vs. Qt; .NET version churn adds cost
  - **Electron / Tauri** — rejected because positioning ("not bloated like Postman") demands a native, lean binary

#### ADR-002 — Two-phase architecture (in-process engine now, extractable later)

- **Status**: Accepted (PRD v0.3)
- **Context**: The dependency-resolution engine is the load-bearing component. CLI mode (FR-13.x), team workspaces, and a future browser version (G9) will all consume the same engine. We must ship MVP fast without painting ourselves into a corner.
- **Decision**: **Phase A** — engine is a pure-C++ library (`libchainapi-engine`) compiled into the Qt desktop app in-process. **Phase B** (post-MVP, kept open) — engine extracted to a separate process or separate language (Rust is the leading candidate) when a second major consumer (CLI in CI, server-side runner) demands it.
- **Rationale**: In-process keeps MVP simple (one build, one binary, one debugger). The architectural guardrails in §8.6 (engine has no Qt-UI dependencies, no signals from engine, language-agnostic serialization) keep the cost of Phase B as a build-system change rather than a rewrite.
- **Trade-offs accepted**: We pay a small upfront discipline tax (no Qt types in the engine layer, narrow adapters for everything) to preserve a path we may not need.
- **Triggers for Phase B**: (a) CLI binary needs to ship without the Qt runtime, (b) second consumer (server / browser) emerges, (c) memory-safety bugs prove painful enough to justify Rust.

#### ADRs to be written

- ADR-003: Why YAML schema over JSON / TOML / custom DSL
- ADR-004: Why local-first over cloud-first
- ADR-005: Why DAG-based execution over linear scripts
- ADR-006: Why open-core over pure-OSS or pure-SaaS

#### ADR-007 — Schema completeness over hook-everything

- **Status**: Accepted (PRD v0.4)
- **Context**: Real APIs have polling/async endpoints, HMAC-signed payloads, OAuth/SigV4 auth, and dynamic timestamps. The original schema (v0.3) handled these by deferring to JS hooks for everything beyond simple username/password auth and JSONPath extraction. Hooks-as-escape-hatch is fast to ship but produces a long tail of YAML files with inline JS that defeats the schema's value proposition.
- **Decision**: Promote the three most common pain points to first-class schema citizens:
  1. Polling becomes `poll_until` on operations (§5.11), not a hook
  2. Common signed-auth schemes (OAuth2, AWS SigV4, OAuth1, API key, Basic) are named strategies (§5.10.1), not hooks
  3. Dynamic-payload primitives (HMAC, JWT, base64, relative timestamps) are built-in template functions (§5.7), not hooks
- **Rationale**:
  - Each promotion eliminates a category of "you need to write JS to test our API" friction
  - The declarative form is reviewable by tooling — extractions, retries, and signing can be linted, diffed, and AI-imported. Hooks are opaque
  - Hooks remain available for the genuine long tail (§5.10), now stored in sibling `.js` files with TypeScript-typed `ctx` for editor support
- **Trade-offs accepted**:
  - More engineering work in MVP — each named auth strategy needs implementation, polling needs a predicate evaluator
  - Schema spec is larger to learn (mitigated: the minimum viable schema in §5.1 stays small; new features are opt-in)
- **Triggers for revisiting**: design-partner feedback that reveals an auth scheme used by ≥ 3 partners that doesn't fit the named strategies — promote that scheme to first class

#### ADR-008 — Verify-before-write for AI-imported schemas

- **Status**: Accepted (PRD v0.4)
- **Context**: AI importers in the wild generate plausible-looking output that fails silently at runtime. Users hit a 404 with no debugging anchor and blame the tool. A 90%-accurate importer is more dangerous than a 60%-accurate one because users *trust* it.
- **Decision**: Make the importer adversarial to its own output. Run a verification pass against sample responses (from OpenAPI examples, captured Postman responses, HAR files, or synthetic LLM-generated samples) for every proposed `extract`. Refuse to write any operation whose extractions are not all ✅ verified. Surface evidence strings instead of confidence scores. Tag every AI-imported entity with `_provenance` so runtime failures get AI-aware diagnostics.
- **Rationale**: Turns silent wrongness into visible wrongness. The first place a user encounters AI output is the review UI — where errors are cheap to fix — not the response panel after a failed run.
- **Trade-offs accepted**: Higher LLM cost per import (verification pass adds ~$0.02). Lower "operations written" rate per import (some are deferred to manual completion). Both are accepted in exchange for trust.
- **Triggers for revisiting**: If the verification pass rejects > 30% of correct extractions on the benchmark, the bar is too strict and the predicate logic needs softening (e.g., type-only verification when sample responses lack the field but the schema declares it).

---

**End of PRD v0.4**

> Next steps: validate the concept (Section 16) before kickoff. Recruit design partners, hand-author the MarketplaceAPI sample, and run the LLM feasibility test for the AI importer.
