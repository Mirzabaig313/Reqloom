---
title: Marketplace API
description: "A complete worked example: 3 actors, 5 resources, 27 operations, bundled with the repo and runnable offline."
---

The sample project shipped with Reqloom, and the one used throughout these docs.
It models a B2C marketplace — vendors list products, customers buy them, admins
handle refunds.

```bash
git clone https://github.com/Mirzabaig313/Reqloom
reqloom lint --project Reqloom/samples/marketplace
```

```ansi
LINT OK — 3 actors, 5 resources, 27 operations. No errors.
```

Linting needs no server, so this works offline. It's the fastest way to confirm
your install is sound.

## Layout

```
samples/marketplace/
├── reqloom.yaml
├── environments/
│   └── local.yaml
├── actors/
│   ├── admin.yaml
│   ├── vendor.yaml
│   └── customer.yaml
└── resources/
    ├── products.yaml
    ├── cart.yaml
    ├── orders.yaml
    ├── refunds.yaml
    └── reviews.yaml
```

```yaml title="reqloom.yaml"
version: 1
name: MarketplaceAPI
description: B2C marketplace backend (sample)
default_environment: local
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

Note the resource **ids** come from `name:` inside each file, so
`resources/orders.yaml` declares `name: order` and its operations are
`order.create`, `order.pay`, and so on — singular.

## Three actors

Each is an identity with its own login. `admin` and `vendor` use email/password;
`customer` uses an OTP chain.

```yaml title="actors/vendor.yaml"
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
  refresh:
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

Two details worth copying into your own schemas. The login extracts `vendor_id`
as well as the token, so operation paths can use
`{{vendor.vendor_id}}` without a separate "who am I" request. And the `refresh`
block means an expired session costs one request instead of a full re-login —
extractions merge, so `vendor_id` survives.

## Environment and secrets

```yaml title="environments/local.yaml"
name: local
variables:
  baseUrl: http://localhost:3000
  admin_email: admin@marketplace.test
  admin_password: !secret ADMIN_PASSWORD
  vendor_email: vendor@marketplace.test
  vendor_password: !secret VENDOR_PASSWORD
  customer_email: customer@marketplace.test
  test_phone: "+15555550100"
```

Emails are committed; passwords are `!secret`, so they come from your OS keychain
and never touch the repo. Note `test_phone` is **quoted** — unquoted it would be
mangled into a number.

## Resources and the chain

Each resource declares what must exist before it can be used. `cart.add_item`
is a good example of both dependency kinds at once:

```yaml title="resources/cart.yaml"
name: cart
description: Customer's shopping cart (one cart per customer)
operations:
  add_item:
    method: POST
    path: /api/v1/cart/items
    actor: customer
    depends_on: [product.publish]
    body:
      product_id: "{{product.product_id}}"
      quantity: 2
    expect_status: 200
    extract:
      cart_item_id: $.data.id
      cart_total: $.data.cart_total
```

`{{product.product_id}}` is an **implicit** dependency — it needs whichever
operation extracts that value. `depends_on: [product.publish]` is **explicit**:
publishing returns nothing this operation reads, but an unpublished product can't
be added to a cart.

## Running the flagship chain

`refund.approve` declares exactly one dependency, and gets six more from the
graph:

```bash
reqloom run refund.approve --project samples/marketplace
```

```ansi
Loaded project: MarketplaceAPI (3 actors, 5 resources)
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create (attempt 1)
  [2] Running: product.publish (attempt 1)
  [3] Running: cart.add_item (attempt 1)
  [4] Running: order.create (attempt 1)
  [5] Running: order.pay (attempt 1)
  [6] Running: refund.request (attempt 1)
  [7] Running: refund.approve (attempt 1)
```

Three actors are involved — vendor for steps 1–2, customer for 3–6, admin for 7 —
and their three logins don't appear in the numbering, because a login happens
inside the step that needs it.

Trace it backwards and the whole chain is one edge per file:

| Step | Needs | Because |
| --- | --- | --- |
| `refund.approve` | `refund.request` | can't approve what doesn't exist |
| `refund.request` | `order.pay` | can't refund an unpaid order |
| `order.pay` | `order.create` | can't pay a nonexistent order |
| `order.create` | `cart.add_item` | can't order an empty cart |
| `cart.add_item` | `product.publish` | can't buy an unpublished product |
| `product.publish` | `product.create` | can't publish nothing |

## There's no server bundled

The sample points at `http://localhost:3000` and the repo doesn't include a
backend, so `reqloom run` will fail on connection unless you point it somewhere
real. That's expected — `lint` is the offline check.

:::caution[Running against nothing is slow]
Each step retries 3 times, and each attempt waits out the 5s connect timeout, so
a 7-step chain against a dead host takes minutes. Shorten it while experimenting:

```yaml title="environments/local.yaml"
transport:
  connect_timeout: 100ms
```
:::

To actually run it, point `baseUrl` at an API implementing these routes:

```bash
reqloom run refund.approve --project samples/marketplace \
  --var baseUrl=https://your-api.example.com
```

## Patterns worth stealing

| Pattern | Where | Why |
| --- | --- | --- |
| Extract an id at login | every actor | Avoids a "who am I" request |
| `refresh` block | `actors/vendor.yaml` | Expiry costs one request, not a re-login |
| `!secret` for passwords | `environments/local.yaml` | Schema stays committable |
| Explicit `depends_on` for state changes | `cart.add_item` | Publish returns nothing but must happen |
| Separate resource per lifecycle | `refunds.yaml` | A refund has its own id and states |
| Quoted phone number | `environments/local.yaml` | Stops YAML retyping it |
| `list_for_customer` / `list_for_vendor` | `orders.yaml` | Same endpoint shape, different actor |

## Reading it yourself

```bash
reqloom lint --project samples/marketplace     # confirm what parses
```

Then open `resources/orders.yaml` — seven operations, and the widest spread of
`depends_on` edges in the project. `resources/reviews.yaml` is the smallest and a
good first read.

## Next

- [Authoring guide](/schema/authoring/) — build one of these from scratch
- [Dependency resolution](/concepts/dependencies/) — how the 7-step chain is derived
- [5-minute tour](/start/tour/) — this project, walked through
