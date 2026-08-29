---
title: Stripe
description: "A validation case study: 24 endpoints modelling Stripe, form-encoded bodies, mandatory idempotency keys, and multi-tenancy via Stripe-Account."
---

The second hand-authored validation: **24 endpoints across 5 resources**
(`customer`, `payment_method`, `payment_intent`, `refund`, `transfer`) and 2 actors
(`platform`, `connected`).

:::note[This is a case study, not a bundled project]
The schema was authored for the validation exercise and isn't shipped — the only
runnable sample is [marketplace](/examples/marketplace/).
:::

## Why Stripe

It breaks assumptions a JSON API doesn't:

- **Form-encoded, not JSON**, with bracket notation for nested fields
- **Mandatory idempotency keys** on every mutating request
- **Multi-tenancy through a header** — same credential, different acting account
- **HTTP Basic with an empty password** — the secret key is the username

## Form-encoded bodies

Stripe accepts `application/x-www-form-urlencoded`. So do Twilio and essentially
every OAuth 2 token endpoint. This was **Finding 5**, and it shipped as
`body_form`:

```yaml
  create:
    method: POST
    path: /v1/customers
    actor: platform
    body_form:
      email: "{{$.faker.email}}"
      name: "Reqloom Test"
      "metadata[source]": "reqloom"
    expect_status: 200
    extract:
      customer_id: $.id
```

Bracket keys are preserved verbatim — quote them in YAML and they arrive as
`metadata[source]=reqloom`, which is what Stripe expects for nested objects.

`body_form` also picks the encoding for you: it's
`application/x-www-form-urlencoded` unless a value starts with `@`, in which case
it becomes multipart. See
[file uploads](/schema/advanced-operations/#file-uploads).

:::caution[Repeated keys aren't supported the way the finding proposed]
The validation asked for `"expand[]": ["latest_charge", "customer"]` to expand into
repeated keys. That **isn't** implemented — `body_form` values are strings, and a
YAML list gets stringified rather than repeated. For Stripe's `expand[]`, use query
params, which is where it worked anyway:

```yaml
    query_params:
      "expand[]": "latest_charge"
```
:::

## Idempotency keys

Stripe requires one on every mutating call. This is the single clearest win over an
HTTP client:

```yaml
    headers:
      Idempotency-Key: "{{$.uuid}}"
```

One line, per operation, generated fresh per request. In Postman this needs a
pre-request script on every endpoint.

## Multi-tenancy: same key, two identities

Stripe Connect acts on behalf of a connected account by adding a `Stripe-Account`
header. The credential is identical; the acting identity is not — so it's **two
actors**:

```yaml title="actors/platform.yaml"
name: platform
description: The platform account itself
auth:
  strategy: basic
  username: "{{secret.STRIPE_SECRET_KEY}}"
  password: ""
```

```yaml title="actors/connected.yaml"
name: connected
description: Acting on behalf of a connected account
auth:
  strategy: basic
  username: "{{secret.STRIPE_SECRET_KEY}}"
  password: ""
inject:
  headers:
    Stripe-Account: "{{env.connected_account_id}}"
```

Switching identity is then one word on an operation:

```yaml
  create_on_behalf:
    method: POST
    path: /v1/charges
    actor: connected          # ← the only difference
```

The validation called this out as the abstraction Stripe Connect needs and that no
existing tool offers. Modelling it as two actors keeps every operation honest about
whose behalf it acts on. See
[one actor per identity](/concepts/actors/#one-actor-per-identity-not-per-credential).

## Basic auth with an empty password

Stripe puts the secret key in the username and leaves the password blank. That
requires `base64(key + ":")`.

**Finding 4** asked for a `!basic` YAML transformer. That wasn't implemented —
what shipped is the `basic` auth strategy, which computes the header for you:

```yaml
auth:
  strategy: basic
  username: "{{secret.STRIPE_SECRET_KEY}}"
  password: ""
```

No `inject:` needed; the `Authorization: Basic …` header is added automatically. If
you ever need the encoding by hand, `{{$.base64.encode('key:')}}` is available.

## The chain

```
customer.create → payment_method.create → payment_method.attach
                → payment_intent.create → payment_intent.confirm
                → refund.create
```

Five levels, which the validation noted is normal in payments and composed without
fanfare. Stripe's test tokens (`tok_visa`) plus a test API key meant the whole
chain ran end-to-end against real Stripe with no mocks — worth remembering when
you're deciding whether to build a mock server.

## What needed no escape hatch

- `expand[]` eager-loading — an ordinary query param
- API version pinning via `Stripe-Version` — an ordinary injected header
- Webhook signature validation — out of test scope, and the legitimate case for a
  `pre_request` HMAC [hook](/schema/advanced-operations/#hooks)

## Takeaways for your own schema

| Stripe pattern | What to use |
| --- | --- |
| Form-encoded body | `body_form:` |
| Nested form field | Quoted bracket key — `"metadata[source]"` |
| Idempotency key | `Idempotency-Key: "{{$.uuid}}"` |
| Secret key as Basic username | `strategy: basic` with `password: ""` |
| Acting on behalf of another account | A second actor with an extra `inject:` header |
| API version pinning | An injected header |
| Repeated query keys | `query_params:`, not `body_form:` |

## Next

- [Marketplace](/examples/marketplace/) — the runnable sample
- [GitHub REST](/examples/github/) — the pre-issued-credential case study
- [Auth strategies](/schema/auth-strategies/) — `basic` and the other ten
