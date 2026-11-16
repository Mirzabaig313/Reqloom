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
**resource**, and a resource collects values from all of its operations:

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
directly as `{{secret.VENDOR_PASSWORD}}`. A project that uses secrets needs an
`id:` in `reqloom.yaml`, because secrets are stored per project. See
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
`$.faker.phone`; any other faker name fails the load. There is no `$.random` or
`$.timestamp`. Full list in the [variable reference](/reference/variables/#builtins).

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
A plain reference means the most recent, and an index picks one:

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

Running an operation again replaces the instances it produced before — in the
desktop app, where values are kept between runs, a second run of
`product.list` does not double the list. And when the latest response returned
`null` for a field, or left it out, a reference to it is unresolved rather than
picking up a value from an earlier run.

:::tip[Let the app write these for you]
In the [desktop app](/desktop/overview/), typing `{{` suggests only the variables
actually in scope at that point, and hovering a row in a response body gives you
**＋ save as variable** — which turns the value you're looking at into an
extraction. Between them, most reference typos never get made.
:::

## When a reference doesn't resolve

The literal text is left in place, not blanked:

```ansi
  FAIL   order.get (12ms) err=E_VAR_UNRESOLVED
```

The operation fails before its request is sent, and the timeline shows the braces
where the value should be. The usual causes:

1. **A typo in the extracted name.** This passes lint — validation checks the
   scope exists, not the field. `{{order.oder_id}}` is a run-time failure.
2. **`{{env.X}}` where you meant `{{$.env.X}}`**, or the reverse. The first reads
   your schema's environment, the second reads the process environment — and
   only for names listed under `allowed_env:`.
3. **A missing secret.** No keychain entry means the variable is unresolved. In
   an operation that is `E_VAR_UNRESOLVED`; in a login body it usually shows up
   as a 401.
4. **The producer returned `null`.** The failure names it: "The last
   user.create response returned null for 'user_id'."

## Where references work

Anywhere a template is accepted: `path`, `headers`, `query_params`, `body`,
`body_form`, `poll_until` paths, actor `auth` config, `inject.headers`, session
`refresh` blocks, and `assert` and `poll_until` conditions. In a condition the
value is substituted as one operand that can't change the expression; see
[templates in predicates](/schema/advanced-operations/#templates-in-predicates).

In a JSON body, a value substituted inside a string is JSON-escaped for you; see
[JSON bodies](/reference/variables/#json-bodies).

Values in a `path` are percent-encoded automatically, and existing `%HH` escapes
are preserved rather than double-encoded.

## Next

- [Variable syntax](/reference/variables/) — the complete grammar and every builtin
- [Sessions & caching](/concepts/sessions/) — how actor values persist
- [Dependency resolution](/concepts/dependencies/) — references as graph edges
