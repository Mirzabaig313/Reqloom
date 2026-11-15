---
title: Variable syntax
description: "The complete {{...}} reference grammar: scopes, resolution order, builtin generators, encoding functions, and indexed references."
---

One sigil, `{{ ... }}`, usable in any path, header, query param, body, form
field, actor auth config, assertion or poll condition. Whitespace inside is
trimmed, so
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

That last point matters. A resource collects variables from all its operations,
so `order.create` extracting `order_id` and `order.pay` extracting `payment_id`
both remain referenceable as `{{order.order_id}}` and `{{order.payment_id}}`.

Two rules keep the newest-first search honest:

- **A re-run replaces.** When an operation runs again, the instances it produced
  before are dropped and its new ones become the newest. Instances from other
  operations keep their place.
- **`null` stops the search.** If the latest response from the operation that
  extracts a field returned `null` for it, or did not contain it, the reference
  is unresolved. It does not fall back to an older instance's value, and the
  failure says why: "The last user.create response returned null for
  'user_id'".

:::caution[An unresolved reference is left in place, not blanked]
If nothing matches, the reference text `{{order.typo}}` stays in place and is
recorded as unresolved. An operation with an unresolved reference fails with
`E_VAR_UNRESOLVED` before its request is sent, and the timeline shows the braces
where the value should be — that's the signal to check the name.
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
| `{{$.env.NAME}}` | A **process** environment variable listed in `allowed_env` |

```yaml
    headers:
      Idempotency-Key: "{{$.uuid}}"
    body:
      email: "{{$.faker.email}}"
      expires_at: "{{$.now + 24h}}"
```

Only `email` and `phone` exist. Any other name — `{{$.faker.name}}`,
`{{$.faker.address}}` — is an error: the project fails to load, and `reqloom lint`
says so:

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'o.a' uses $.faker.name, which is not a
faker generator; valid names: email, phone
```

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
`{{order.order_id}}`. It is checked at load like any other reference: an unknown
scope inside a function call fails with `E_REF_UNDEFINED`, and a resource
referenced there becomes a dependency.

## `env` vs `$.env`

Similar names, different sources. This trips people up:

```yaml
environment:
  region: us-east-1
```

- `{{env.region}}` → `us-east-1`, from the schema
- `{{$.env.region}}` → the shell's `$region`, via `getenv` — and only if
  `region` is listed under `allowed_env:` in `reqloom.yaml`

Use `{{env.X}}` for project configuration and `{{$.env.X}}` only to pull a value
the CI runner already has in its environment:

```yaml title="reqloom.yaml"
allowed_env:
  - CI_API_TOKEN
```

A `$.env` name that isn't in `allowed_env` fails the load with `E_REF_UNDEFINED`.
Values read through `$.env` are masked in output like secrets. See
[`{{secret.X}}` vs `{{$.env.X}}`](/schema/secrets-and-transport/#secretx-vs-envx).

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

## JSON bodies

When a body is sent as JSON, a value substituted **inside a JSON string** is
escaped as string content, so quotes, backslashes, and newlines in the value
can't break the document:

```yaml
    body:
      note: "{{env.note}}"       # env.note = He said "hi"
```

```json
{"note":"He said \"hi\""}
```

A value **outside** a string is inserted exactly as written, so numbers,
booleans, arrays, and objects still work. In a structured `body:` every value is
already a JSON string, so use a scalar body for this:

```yaml
    body: |
      {"quantity": {{env.qty}}, "tags": {{env.tags_json}}}
```

The body counts as JSON when it starts with `{` or `[` and either has no
`Content-Type` header or one that is `application/json` or ends in `+json`.
Other bodies — form data, XML, plain text — get plain substitution.

Write values unescaped. A value you escaped by hand (`He said \"hi\"`) is escaped
again and arrives with the backslashes in it.

## URL encoding

In a `path`, resolved values are percent-encoded automatically, and existing
`%HH` escapes are preserved rather than double-encoded. The path and query are
handled separately, split at the first unbraced `?`.

For safety, a path that is one whole reference must resolve to something starting
with a single `/` and containing no backslashes or control characters — otherwise
it's rejected as unresolved rather than sent.

## Where references work

Everywhere a template is accepted: `path`, `headers`, `query_params`, `body`,
`body_form`, the `poll_until` path, actor `auth` config, `inject.headers`, and
session `refresh` blocks.

In predicates — `assert`, `poll_until.success_when` and `poll_until.fail_when`
— each placeholder becomes one value: escaped for the string literal it sits in,
or, outside a literal, a bare number, `true`, `false` or `null` when the value is
exactly that and a quoted string otherwise. A value can never change the
structure of the predicate. A poll condition is resolved once, before the first
attempt. See [templates in predicates](/schema/advanced-operations/#templates-in-predicates).

## Next

- [Variables & references](/concepts/variables/) — the concept, with worked examples
- [Advanced operations](/schema/advanced-operations/) — `for_each` and instances
- [Secrets](/schema/secrets-and-transport/#secrets) — the `!secret` tag
