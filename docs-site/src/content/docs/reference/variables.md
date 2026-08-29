---
title: Variable syntax
description: "The complete {{...}} reference grammar: scopes, resolution order, builtin generators, encoding functions, and indexed references."
---

One sigil, `{{ ... }}`, usable in any path, header, query param, body, form
field, actor auth config, or predicate. Whitespace inside is trimmed, so
`{{ order.order_id }}` and `{{order.order_id}}` are the same.

## Scopes

| Scope | Resolves from | Example |
| --- | --- | --- |
| `$.` | Builtin generators and functions | `{{$.uuid}}`, `{{$.now}}` |
| `env` | Your schema's `environment:` block | `{{env.baseUrl}}` |
| `secret` | OS keychain | `{{secret.STRIPE_KEY}}` |
| `<actor>` | That actor's session variables | `{{vendor.token}}` |
| `<resource>` | The most recent instance's extractions | `{{order.order_id}}` |
| `<resource>[N]` | A specific instance, **1-indexed** | `{{order[2].order_id}}` |

A reference with no dot never resolves — `{{token}}` is always literal text.

## Resolution order

Checked in this order, first match wins:

1. **Builtins** — anything starting with `$.`
2. **`env`** — project environment variables
3. **`secret`** — keychain
4. **Actor sessions** — when the scope names an actor
5. **Indexed resource** — when the scope looks like `name[N]`
6. **Resource instances** — searched **newest first**

That last point matters. A resource accumulates variables across operations, so
`order.create` extracting `order_id` and `order.pay` extracting `payment_id`
both remain referenceable as `{{order.order_id}}` and `{{order.payment_id}}`.

:::caution[An unresolved reference is left in place, not blanked]
If nothing matches, the literal text `{{order.typo}}` is sent as-is and recorded
as unresolved. You'll see a request URL with braces in it rather than an empty
segment — that's the signal to check the name.
:::

## Builtins

### Generators

| Reference | Produces |
| --- | --- |
| `{{$.uuid}}` | A UUID v4 |
| `{{$.now}}` | ISO-8601 UTC, second precision |
| `{{$.now + 1h}}` | Offset timestamp — `s`, `m`, `h`, `d`, signed |
| `{{$.faker.email}}` | `test+<hex>@example.com` |
| `{{$.faker.phone}}` | A `+1555…` number |
| `{{$.env.NAME}}` | A **process** environment variable |

```yaml
    headers:
      Idempotency-Key: "{{$.uuid}}"
    body:
      email: "{{$.faker.email}}"
      expires_at: "{{$.now + 24h}}"
```

Only `email` and `phone` are real faker generators. Any other type — 
`{{$.faker.name}}`, `{{$.faker.address}}` — returns a placeholder shaped like
`faker_name_1a2b3c4d`. It never fails, which means a typo produces junk rather
than an error.

:::note[There is no `$.random`, `$.timestamp`, or `$.date`]
`$.uuid` and `$.now` are the only generators, plus faker. Offsets work on
`$.now` only.
:::

### Encoding functions

| Reference | Does |
| --- | --- |
| `{{$.base64.encode(x)}}` | Base64 encode |
| `{{$.base64.decode(x)}}` | Base64 decode |
| `{{$.hex.encode(x)}}` | Hex encode |
| `{{$.hex.decode(x)}}` | Hex decode |
| `{{$.url.encode(x)}}` | Percent-encode |
| `{{$.url.decode(x)}}` | Percent-decode |

The argument is either a quoted literal or another reference:

```yaml
    headers:
      Authorization: "Basic {{$.base64.encode('user:pass')}}"
      X-Encoded-Id: "{{$.url.encode(order.order_id)}}"
```

Note the inner reference is written **without** braces — `order.order_id`, not
`{{order.order_id}}`.

## `env` vs `$.env`

Similar names, different sources. This trips people up:

```yaml
environment:
  region: us-east-1
```

- `{{env.region}}` → `us-east-1`, from the schema
- `{{$.env.region}}` → the shell's `$region`, via `getenv`, usually nothing

Use `{{env.X}}` for project configuration and `{{$.env.X}}` only to pull a value
the CI runner already has in its environment.

An `env` value may itself contain a reference, and is expanded one level further:

```yaml
environment:
  baseUrl: https://api.example.com
  ordersUrl: "{{env.baseUrl}}/orders"
```

Nesting is capped at 4 levels. If the inner reference fails, the whole outer
reference is reported unresolved rather than half-expanded.

## `baseUrl` is special

`baseUrl` is the one environment variable the engine treats as magic: the request
URL is `baseUrl` + the operation's `path`.

```yaml title="environments/local.yaml"
name: local
variables:
  baseUrl: http://localhost:3000
```

```yaml
  get:
    path: /api/v1/orders/{{order.order_id}}
```

That requests `http://localhost:3000/api/v1/orders/ord_123`. Note the spelling —
**camelCase**, not `base_url`. With no `baseUrl` set, the path is used alone,
which almost never works and produces a confusing failure. There is no `url:`
key on an operation.

## Indexed references

When an extraction uses `[*]`, each match becomes a separate instance:

```yaml
    extract:
      product_id: $.data[*].id
```

Reference a specific one, **1-indexed**:

```yaml
    path: /api/v1/products/{{product[1].product_id}}
```

`{{product[0].x}}` never resolves — counting starts at 1. An out-of-range index
resolves to nothing and surfaces as `E_VAR_UNRESOLVED`.

Inside a [`for_each`](/schema/advanced-operations/#fanning-out-with-for_each),
the plain form `{{product.product_id}}` binds to the current iteration instead of
the newest instance.

## Cross-operation references use the resource

Extractions belong to the **resource**, not the operation that produced them. So
if `order.create` extracts `order_id`:

```yaml
    path: /api/v1/orders/{{order.order_id}}/pay      # correct
    path: /api/v1/orders/{{order.create.order_id}}/pay   # wrong — never resolves
```

Referencing a resource creates an implicit dependency, which is how chains build
themselves. `depends_on` is only needed for prerequisites that don't hand you a
value.

## URL encoding

In a `path`, resolved values are percent-encoded automatically, and existing
`%HH` escapes are preserved rather than double-encoded. The path and query are
handled separately, split at the first unbraced `?`.

For safety, a path that is one whole reference must resolve to something starting
with a single `/` and containing no backslashes or control characters — otherwise
it's rejected as unresolved rather than sent.

## Where references work

Everywhere a template is accepted: `path`, `headers`, `query_params`, `body`,
`body_form`, `expect_status`-adjacent predicates (`assert`,
`poll_until.success_when` / `fail_when`), actor `auth` config, `inject.headers`,
and session `refresh` blocks.

## Next

- [Variables & references](/concepts/variables/) — the concept, with worked examples
- [Advanced operations](/schema/advanced-operations/) — `for_each` and instances
- [Secrets](/schema/secrets-and-transport/#secrets) — the `!secret` tag
