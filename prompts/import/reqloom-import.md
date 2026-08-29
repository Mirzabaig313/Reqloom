# Reqloom Import — Single Prompt

Paste this whole file into your model, then paste your API description (OpenAPI
spec, Postman export, Markdown docs, or curl logs) after it.

One prompt, one pass. Everything the model needs — the schema reference, a worked
example, and the rules — is below, so it has to invent very little.

---

## Your task

You are converting an API description into a **Reqloom project**: a set of YAML
files that describe the API as a graph of actors, resources, and dependencies, so
a single command can run any endpoint and its prerequisites automatically.

Work in two parts, in one response:

1. **A plan** in plain English, plus any questions you need answered.
2. **The YAML files**, only for the parts you are confident about.

If you have file-writing tools, write the files to disk. Otherwise output each
one in a fenced block labelled with its path.

---

## Non-negotiable rules

1. **Never invent a path, field name, or status code.** If the input doesn't say
   it, put it under **Open questions** instead. A wrong `expect_status` or a
   hallucinated body field is worse than a gap, because it looks correct.
2. **Ask before guessing.** Any inference you cannot ground in the input becomes
   an open question. "The path contains `{order_id}`, so it needs an order" is
   grounded. "This probably needs a user first" is a question.
3. **Omit rather than fill.** No `depends_on: []`, no `actor: ""`, no placeholder
   body fields. Leave the key out entirely.
4. **Only the keys in the reference below exist.** Anything else is silently
   ignored by the parser, so a misspelling fails quietly. Do not invent keys.
5. **Mark every inference** with `_provenance.evidence` so a human can audit it.

---

## The Reqloom schema

### File structure

Produce exactly this layout. One file per actor, one per resource, one per
environment:

```
my-api/
├── reqloom.yaml                  project root: version, name, imports
├── environments/
│   └── local.yaml                baseUrl + variables + !secret placeholders
├── actors/
│   ├── admin.yaml                one file per identity
│   ├── vendor.yaml
│   └── customer.yaml
└── resources/
    ├── products.yaml             one file per domain entity
    ├── orders.yaml
    └── refunds.yaml
```

**Directory names are load-bearing.** An imported file is parsed as an actor, a
resource, or an environment based on its path prefix — `actors/`, `resources/`,
`environments/`. Those three are the only recognised prefixes, and a file
imported from anywhere else is loaded and then **silently discarded**. Do not
invent `shared/`, `common/`, `auth/`, or `config/` directories.

Naming rules:

| Item | File | Id comes from |
|---|---|---|
| Project root | `reqloom.yaml` | — must be this exact name |
| Environment | `environments/<env>.yaml` | `name:` inside, else the filename stem |
| Actor | `actors/<actor>.yaml` | `name:` inside, else the filename stem |
| Resource | `resources/<plural>.yaml` | `name:` inside, else the filename stem |

The resource **id** is what operations and references use, and it comes from
`name:` — not the filename. So `resources/orders.yaml` declaring `name: order`
gives operations `order.create`, `order.pay`, and references
`{{order.order_id}}`. Prefer a plural filename with a singular `name:`, matching
the example above.

Everything is flat — there is no nesting below these directories, because the
import globs do not recurse.

### Project root — `reqloom.yaml`

```yaml
version: 1                      # required, 1–3
name: My API
default_environment: local
imports:                        # only a trailing *.yaml glob expands
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

`imports:` is required for a multi-file project — without it, nothing in those
directories is loaded and you get an empty project that lints clean. Only a
trailing `*.yaml` or `*.yml` expands; `**` and `?` are treated as literal
filenames and match nothing.

For a very small API (one actor, a handful of operations) you may instead inline
`environment:`, `actors:`, and `resources:` as maps in `reqloom.yaml` and omit
`imports:` entirely. Prefer the multi-file layout above once there is more than
one actor or more than about three resources.

### Environment — `environments/local.yaml`

```yaml
name: local
variables:
  baseUrl: http://localhost:3000        # camelCase, and magic: URL = baseUrl + path
  admin_email: admin@example.com
  admin_password: !secret ADMIN_PASSWORD
```

- `baseUrl` ends at the **host**. Do not include `/api/v1` — that belongs in each
  operation's `path`.
- `!secret NAME` binds the value to the OS keychain. Use it for every password,
  token, and API key. It is the only YAML tag that exists.
- For values the user supplies per run, write a visible placeholder like
  `<SET_VIA_--var>`.

### Actor — `actors/<id>.yaml`

An actor is an identity: how to log in, how long the session lasts, what to attach
to every request.

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
  ttl: 15m
  refresh:                              # optional — one request instead of re-login
    method: POST
    path: /api/v1/auth/refresh
    body:
      refresh_token: "{{vendor.refresh_token}}"
    extract:
      token: $.data.accessToken
inject:
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

`strategy:` must be exactly one of:

| Strategy | Keys it reads |
|---|---|
| `simple` | `method`, `path`, `headers`, `body`, `expect_status`, `extract` |
| `chain` | `steps:` — a sequence of the same keys, each with an `id` |
| `basic` | `username`, `password` |
| `api_key` | `key`, `location` (`header`\|`query`\|`cookie`), `name` |
| `bearer` | `token` |
| `oauth2_client_credentials` | `token_url`, `client_id`, `client_secret`, `scope` |
| `oauth2_password` | the above plus `username`, `password` |
| `oauth1` | `consumer_key`, `consumer_secret`, `token`, `token_secret`, `realm` |
| `aws_sigv4` | `access_key`, `secret_key`, `region`, `service`, `session_token` |
| `jwt` | `secret`, `payload`, `algorithm` |
| `mtls` | `cert_path`, `key_path`, `key_password`, `ca_cert_path`, `format` |

An unrecognised strategy silently becomes `simple`. Note it is
`oauth2_client_credentials`, never `oauth2`.

For a **pre-issued** credential — a personal access token, an API key — there is no
login to model. Use `bearer` or `api_key` and skip `inject:`:

```yaml
name: service
auth:
  strategy: bearer
  token: "{{secret.SERVICE_TOKEN}}"
session:
  ttl: 24h
```

Multi-step logins (OTP, MFA) use `chain`:

```yaml
auth:
  strategy: chain
  steps:
    - id: request_otp
      method: POST
      path: /api/v1/auth/otp/request
      body: { phone: "{{env.test_phone}}" }
      expect_status: 200
    - id: verify_otp
      method: POST
      path: /api/v1/auth/otp/verify
      body: { phone: "{{env.test_phone}}", code: "{{env.test_otp}}" }
      expect_status: 200
      extract:
        token: $.data.accessToken
```

### Resource — `resources/<id>.yaml`

```yaml
name: order                     # the id. Singular. Operations become order.<op>
description: Customer orders
operations:
  create:
    method: POST                # default GET; an unknown value silently becomes GET
    path: /api/v1/orders        # appended to baseUrl. There is no `url:` key
    actor: customer             # omit entirely for a public endpoint
    depends_on: [cart.add_item] # must be a sequence; omit if empty
    headers:
      Content-Type: application/json
    query_params:               # NOT `query:` or `params:`
      limit: "20"
    body:
      total: 100
    expect_status: 201          # or a sequence: [200, 202]
    extract:
      order_id: $.data.id
    assert:                     # must be a sequence, or it is ignored
      - "$.data.status == 'pending'"
```

Other operation keys: `body_form` (form-encoded; `@./path` for uploads),
`poll_until`, `for_each`, `auth` (per-operation inline credential), `timeout`
(integer **milliseconds**), `retry: { max, backoff }` (`backoff` in ms), `force`,
`pre_request` / `post_response` (JS hook, inline or a relative `./x.js` path).

**Extraction sources** are inferred from the path prefix:

| Path | Reads |
|---|---|
| `$.data.id` | JSON body |
| `$.headers.Location` | a response header |
| `$.cookies.session` | a cookie |
| `$.status_code` | the status code |
| `$.data[*].id` | a list — one resource instance per match |

XPath and regex need the explicit form:

```yaml
    extract:
      order_ref:
        path: 'ref-([0-9]+)'
        source: regex
```

### References

One sigil, `{{ ... }}`. Resolution order: builtins → `env` → `secret` → actor
session → indexed resource → resource (newest first).

| Form | Means |
|---|---|
| `{{order.order_id}}` | a value extracted by any operation of resource `order` |
| `{{vendor.token}}` | a value from actor `vendor`'s login |
| `{{env.baseUrl}}` | an environment variable |
| `{{secret.API_KEY}}` | a keychain secret |
| `{{$.uuid}}` | a fresh UUID v4 |
| `{{$.now}}` / `{{$.now + 24h}}` | ISO-8601 timestamp, optional offset |
| `{{$.faker.email}}` / `{{$.faker.phone}}` | generated test data |
| `{{$.base64.encode('a:b')}}` | base64, also `$.hex` and `$.url` |
| `{{order[2].order_id}}` | a specific instance, **1-indexed** |

**Extractions belong to the resource, not the operation.** If `order.create`
extracts `order_id`, reference it as `{{order.order_id}}`. There is no
`{{order.create.order_id}}` form.

Only `$.uuid`, `$.now`, and `$.faker.*` generate values. There is no `$.random`,
`$.timestamp`, or `$.date`.

### Dependencies

Two kinds, and you need both:

- **Implicit** — writing `{{order.order_id}}` means "whichever operation extracts
  this must run first". This is most of the graph. Prefer it.
- **Explicit** — `depends_on: [product.publish]` for a prerequisite that returns
  nothing you read but must still happen. State changes (publish, activate,
  approve, verify) are the usual case.

Never create a cycle. If A needs a value from B and B needs one from A, one of
those edges is wrong.

---

## Worked example

Input (abridged API docs):

```
POST /api/v1/auth/vendor/login   {email, password} → 200 {data:{accessToken, user:{id}}}
POST /api/v1/products            {name, price}     → 201 {data:{id}}   (vendor only)
POST /api/v1/products/{id}/publish                 → 200                (vendor only)
```

Output:

```yaml
# reqloom.yaml
version: 1
name: Marketplace
default_environment: local
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

```yaml
# environments/local.yaml
name: local
variables:
  baseUrl: http://localhost:3000
  vendor_email: vendor@example.com
  vendor_password: !secret VENDOR_PASSWORD
```

```yaml
# actors/vendor.yaml
name: vendor
description: Vendor authenticated with email and password
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
    vendor_id: $.data.user.id
session:
  ttl: 15m
inject:
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

```yaml
# resources/products.yaml
name: product
description: Vendor product catalogue
operations:
  create:
    method: POST
    path: /api/v1/products
    actor: vendor
    body:
      name: "Test product {{$.uuid}}"
      price: 1000
    expect_status: 201
    extract:
      product_id: $.data.id
    _provenance:
      source: ai_import
      evidence:
        actor: "docs mark this endpoint vendor-only"
  publish:
    method: POST
    path: /api/v1/products/{{product.product_id}}/publish
    actor: vendor
    depends_on: [product.create]
    expect_status: 200
```

Note what happened: `publish` references `{{product.product_id}}`, so the
dependency on `create` is implicit — the `depends_on` is belt-and-braces and could
be omitted. `price` is a number because the docs imply one; `name` is unique per
run via `{{$.uuid}}`.

---

## Required output format

### Part 1 — Plan

```markdown
## Actors
- <id>: who they are, which strategy, what the login returns

## Resources and operations
| operation | method | path | actor | depends_on | expect_status | extracts |
|---|---|---|---|---|---|---|

## Variable flow
| variable | produced by | consumed by |
|---|---|---|

## Environment variables
| variable | kind | notes |
|---|---|---|
| baseUrl | plain | host only |
| admin_password | secret | !secret ADMIN_PASSWORD |

## Open questions
- Numbered, specific, and blocking. Every ungrounded inference goes here.
```

### Part 2 — Files

`reqloom.yaml`, then `environments/local.yaml`, then one file per actor, then one
per resource.

**Skip anything a question blocks.** A project with 6 solid resources and 3 flagged
gaps is more useful than 9 resources where 3 are invented.

---

## Self-check before responding

- [ ] Does every path, field, and status code trace to the input, or appear under
      Open questions?
- [ ] Is every `{{X.y}}` scope a real actor, resource, `env`, or `secret`?
- [ ] Does every referenced `{{env.X}}` appear in the environment file?
- [ ] Are extractions referenced by **resource**, not by operation?
- [ ] Is every password, token, and key a `!secret` or `{{secret.X}}`?
- [ ] Does `baseUrl` stop at the host, with no version prefix?
- [ ] Are `depends_on` and `assert` sequences, and omitted when empty?
- [ ] Is `actor:` omitted entirely on public endpoints?
- [ ] Are there no cycles?
- [ ] Is `strategy:` one of the eleven exact values?

Fix anything that fails before you respond.

---

## Then: validate and iterate

The user runs:

```bash
reqloom lint --project <dir>
```

Paste the output back. Common errors and their fixes:

| Error | Cause | Fix |
|---|---|---|
| `E_YAML_PARSE` | Bad indentation or quoting | The message names file and line |
| `E_SCHEMA_VERSION` | `version:` missing or outside 1–3 | Set `version: 1` |
| `E_REF_UNDEFINED` | A `{{X.y}}` scope or `depends_on` target doesn't exist | Add the producer, or fix the name |
| `E_CYCLE` | Circular dependency | Remove the needless `depends_on` — the message prints the path |
| `E_VAR_UNRESOLVED` | Runtime: nothing produced that value | Usually a typo'd extraction name |
| `E_STATUS_MISMATCH` | Wrong `expect_status` | Use the real status; `[202, 200]` for async |

Edit the existing files — do not regenerate from scratch.

**A clean lint does not mean a correct schema.** Lint validates structure. It
cannot tell whether a path, body, or status matches the real API, and it checks
that a reference's *scope* exists rather than the field — so `{{order.typo}}`
passes. One real run per chain is the only proof. When a response confirms an
operation, update its provenance:

```yaml
    _provenance:
      verified_against: live_capture     # was: synthetic
```
