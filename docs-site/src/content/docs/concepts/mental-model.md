---
title: The mental model
description: "Four ideas — actors, resources, dependencies, variables. Understand these and the rest of Reqloom is detail."
---

Reqloom models your API as a graph. Four ideas make up the whole thing; polling,
assertions, retries, and hooks are detail layered on top.

| Idea | Answers |
| --- | --- |
| **Actor** | Who is making this request? |
| **Resource** | What thing does this endpoint act on? |
| **Dependency** | What must happen before it? |
| **Variable** | How does a value get from one response into the next request? |

## 1. Actors — who

An **actor** is an identity your API recognises: `admin`, `vendor`, `customer`, a
worker service. You declare how to become it once.

```yaml title="actors/admin.yaml"
name: admin
description: Marketplace admin (email + password)
auth:
  strategy: simple
  method: POST
  path: /api/v1/auth/admin/login
  body:
    email: "{{env.admin_email}}"
    password: "{{env.admin_password}}"
  expect_status: 200
  extract:
    token: $.data.accessToken
    admin_id: $.data.user.id
session:
  ttl: 15m
inject:
  headers:
    Authorization: "Bearer {{admin.token}}"
```

Three parts: `auth` says how to log in and what to keep from the response,
`session` says how long to reuse it, `inject` says what every request as this
actor carries.

Any operation can then say `actor: admin` and get credentials automatically.
Anything the login extracted is available too — `{{admin.admin_id}}`, not just the
token. There are [eleven auth strategies](/schema/auth-strategies/) for the
different shapes real APIs use.

## 2. Resources — what

A **resource** is a thing your API manages, grouping the operations that act on it.

```yaml title="resources/orders.yaml"
name: order
description: Customer orders
operations:
  create:
    method: POST
    path: /api/v1/orders
    actor: customer
    expect_status: 201
    extract:
      order_id: $.data.id
  pay:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/pay
    actor: customer
    depends_on: [order.create]
    expect_status: 200
    extract:
      payment_id: $.data.payment.id
```

The resource id comes from `name:` — so this file gives you `order.create` and
`order.pay`, and the filename is irrelevant.

**The resource owns the extracted values.** `create` produced `order_id`, `pay`
produced `payment_id`, and both are now `{{order.order_id}}` and
`{{order.payment_id}}`. There is no `{{order.create.order_id}}` form. This is the
piece that makes everything else work.

## 3. Dependencies — what first

Two kinds of edge, and you need both:

**Implicit** — a reference. Writing `{{order.order_id}}` in `pay`'s path says
"whatever produces this must run first". Most of your graph comes from this, free.

**Explicit** — `depends_on`, for a prerequisite that returns nothing you read but
must still happen:

```yaml title="resources/cart.yaml"
  add_item:
    method: POST
    path: /api/v1/cart/items
    actor: customer
    depends_on: [product.publish]      # publish returns nothing we use,
    body:                              # but an unpublished product can't be bought
      product_id: "{{product.product_id}}"
      quantity: 2
```

State changes — publish, activate, approve, verify — are the usual reason to reach
for `depends_on`.

From those edges the engine builds a chain, sorts it topologically with a
deterministic tie-break, and runs it. Ask for a shallow endpoint and you get a
shallow chain:

```bash
reqloom run product.list      # chain of 1 steps
reqloom run cart.add_item     # chain of 3 steps
reqloom run order.pay         # chain of 5 steps
reqloom run refund.approve    # chain of 7 steps
```

Reqloom adds nothing when nothing is needed. `product.list` is a public listing
endpoint with no prerequisites, so it stays one request.

## 4. Variables — how values move

One sigil, `{{ ... }}`. Six scopes:

```yaml
    path: /api/v1/orders/{{order.order_id}}/pay   # a resource's extracted value
    headers:
      Authorization: "Bearer {{vendor.token}}"    # an actor's session value
      Idempotency-Key: "{{$.uuid}}"               # a builtin generator
    body:
      email: "{{env.customer_email}}"             # an environment variable
      api_key: "{{secret.STRIPE_KEY}}"            # an OS keychain secret
      first: "{{order[1].order_id}}"              # a specific instance, 1-indexed
```

Resolution order is builtins → `env` → `secret` → actor sessions → indexed
resource → resource. An unresolved reference is left in place verbatim, so you see
braces in the URL rather than a silently empty segment.

## Putting it together

That's the model. Running one endpoint:

<div class="not-content">

1. Parse the schema and validate every reference and `depends_on` target
2. Build the graph, sort it, detect cycles
3. For each step: authenticate the actor if its session has expired, substitute
   `{{...}}`, send, extract into the resource, evaluate assertions
4. Report each step with a status and, on failure, an `E_*` code

</div>

```ansi
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create (attempt 1)
  [2] Running: product.publish (attempt 1)
  [3] Running: cart.add_item (attempt 1)
  [4] Running: order.create (attempt 1)
  [5] Running: order.pay (attempt 1)
  [6] Running: refund.request (attempt 1)
  [7] Running: refund.approve (attempt 1)

Result: SUCCEEDED
```

Three actors took part in those seven steps. **Their logins are not steps** — a
login belongs to whichever step needs it, and is cached for its TTL. Seven steps
across three identities cost three logins.

## What the model buys you

- **Any endpoint, one command**, however deep its prerequisites
- **Auth declared once** per identity instead of per request
- **A schema that is also documentation** — and reviewable in a pull request
- **One schema, every environment** — switch with `--env`
- **A CI test runner** with JUnit output and meaningful exit codes

## What it isn't

- **A scripting platform.** The schema is declarative; [hooks](/schema/advanced-operations/#hooks)
  are the escape hatch, not the model.
- **A flow builder.** The dependency graph is generated *from* the schema, not the
  other way round.
- **GUI-first.** The YAML is the source of truth; the desktop app is a view onto
  it.

## Next

- [Actors](/concepts/actors/) — the eleven auth strategies and session behaviour
- [Resources & operations](/concepts/resources/) — boundaries and extraction
- [Dependency resolution](/concepts/dependencies/) — ordering, cycles, caching
- [Variables & references](/concepts/variables/) — the full grammar
- [Authoring guide](/schema/authoring/) — build one from scratch
