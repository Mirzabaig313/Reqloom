---
title: Variables & references
description: "How values move between operations: extract on the way out, reference on the way in, and the six scopes a reference can come from."
---

A chain is only useful if values flow along it. An order id created in step 4 has
to reach step 5. That's two halves: **extract** a value from a response, then
**reference** it later.

## Extract, then reference

```yaml title="resources/orders.yaml"
name: order
operations:
  create:
    method: POST
    path: /api/v1/orders
    actor: customer
    expect_status: 201
    extract:
      order_id: $.data.id           # ← pull it out
  pay:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/pay    # ← use it
    actor: customer
    depends_on: [order.create]
```

`extract` names the value; `{{order.order_id}}` uses it. Referencing it is also
what tells the engine `order.create` has to run first — see
[dependency resolution](/concepts/dependencies/).

## Values belong to the resource

Note the reference is `{{order.order_id}}`, not `{{order.create.order_id}}`. There
is no per-operation form. Everything an operation extracts is stored on its
**resource**, and a resource accumulates values as its operations run:

```yaml
  # order.create extracted order_id
  # order.pay extracted payment_id
  # both are available now:
  refund:
    path: /api/v1/orders/{{order.order_id}}/refunds
    body:
      payment: "{{order.payment_id}}"
```

## The six scopes

| Scope | Comes from | Example |
| --- | --- | --- |
| `<resource>` | An operation's `extract` | `{{order.order_id}}` |
| `<actor>` | An actor's login response | `{{vendor.token}}` |
| `env` | The active environment | `{{env.baseUrl}}` |
| `secret` | Your OS keychain | `{{secret.API_KEY}}` |
| `$.` | Builtin generators | `{{$.uuid}}`, `{{$.now}}` |
| `<resource>[N]` | A specific instance, 1-indexed | `{{order[2].order_id}}` |

A reference always has a dot. `{{token}}` on its own never resolves — it's sent
as literal text.

### Actor values

Auth extractions work exactly like operation extractions:

```yaml title="actors/vendor.yaml"
auth:
  strategy: simple
  path: /api/v1/auth/vendor/login
  extract:
    token: $.data.accessToken
    vendor_id: $.data.user.id
```

Both are usable anywhere — not just in `inject`:

```yaml
  create:
    path: /api/v1/vendors/{{vendor.vendor_id}}/products
    actor: vendor
```

### Environment values

Per-environment configuration, so one schema runs everywhere:

```yaml title="environments/local.yaml"
name: local
variables:
  baseUrl: http://localhost:3000
  vendor_email: vendor@marketplace.test
```

```bash
reqloom run order.create --env staging
reqloom run order.create --var baseUrl=http://localhost:4000    # one-off override
```

`baseUrl` is the one magic name: every request URL is `baseUrl` + the operation's
`path`. Spelled camelCase — `base_url` is just an ordinary unused variable.

### Secrets

`!secret` binds a variable to the OS keychain, so the schema stays committable:

```yaml
variables:
  vendor_password: !secret VENDOR_PASSWORD
```

Reference it like any other environment variable — `{{env.vendor_password}}` — or
directly as `{{secret.VENDOR_PASSWORD}}`. See
[secrets](/schema/secrets-and-transport/#secrets).

### Builtins

```yaml
    headers:
      Idempotency-Key: "{{$.uuid}}"
    body:
      email: "{{$.faker.email}}"
      expires_at: "{{$.now + 24h}}"
```

`{{$.uuid}}` and `{{$.now}}` are the only generators, plus `$.faker.email` and
`$.faker.phone`. There is no `$.random` or `$.timestamp`. Full list in the
[variable reference](/reference/variables/#builtins).

## Unique values, two different needs

A distinction worth getting right:

```yaml
    body:
      # unique per request, generated
      idempotency_key: "{{$.uuid}}"
      # unique per run, supplied from outside
      coupon: "{{env.coupon_code}}"
```

Use `{{$.uuid}}` when you need *something* unique and don't care what. Use an
environment variable with `--var` when the value has to be known to the caller —
a coupon code you'll verify later, a tenant you're targeting.

## Instances and indexing

Each time an operation extracts values it creates an **instance** of its resource.
Run `order.create` three times and there are three orders. A plain reference means
the most recent:

```yaml
    path: /api/v1/orders/{{order.order_id}}          # newest
    path: /api/v1/orders/{{order[2].order_id}}       # the second one
```

Indexing starts at **1**. `{{order[0].x}}` never resolves.

Extracting with `[*]` makes many instances at once, which is what
[`for_each`](/schema/advanced-operations/#fanning-out-with-for_each) iterates:

```yaml
    extract:
      product_id: $.data[*].id       # one instance per element
```

## When a reference doesn't resolve

The literal text is left in place, not blanked:

```ansi
  FAIL   order.get (12ms) err=E_VAR_UNRESOLVED
```

You'll see braces in the request URL. The usual causes:

1. **A typo in the extracted name.** This passes lint — validation checks the
   scope exists, not the field. `{{order.oder_id}}` is a run-time failure.
2. **`{{env.X}}` where you meant `{{$.env.X}}`**, or the reverse. The first reads
   your schema's environment, the second reads the process environment.
3. **A missing secret.** No keychain entry means the variable is unresolved, and
   you typically get a 401 rather than a clear error.

## Where references work

Anywhere a template is accepted: `path`, `headers`, `query_params`, `body`,
`body_form`, `assert` predicates, `poll_until` paths and predicates, actor `auth`
config, `inject.headers`, and session `refresh` blocks.

Values in a `path` are percent-encoded automatically, and existing `%HH` escapes
are preserved rather than double-encoded.

## Next

- [Variable syntax](/reference/variables/) — the complete grammar and every builtin
- [Sessions & caching](/concepts/sessions/) — how actor values persist
- [Dependency resolution](/concepts/dependencies/) — references as graph edges
