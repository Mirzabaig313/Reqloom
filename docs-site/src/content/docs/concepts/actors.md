---
title: Actors
description: "An actor is an identity: how to log in as it, how long its session lasts, and what to attach to every request it makes."
---

An **actor** is an identity your API recognises — an admin, a vendor, a customer,
a worker service. You describe how to become that identity once, and every
operation that runs as it gets credentials automatically.

That's the difference between Reqloom and an HTTP client. In Postman you paste a
token into a variable and refresh it by hand when it expires. Here, the login is
part of the schema and the engine runs it when it's needed.

## What an actor holds

```yaml title="actors/vendor.yaml"
name: vendor
description: Marketplace vendor with email/password auth

auth:                                   # how to become this identity
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

session:                                # how long credentials stay valid
  ttl: 15m

inject:                                 # what to attach to every request
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

Three parts, and each answers a separate question:

- **`auth`** — what request(s) produce a session, and what to pull out of the
  response
- **`session`** — how long to reuse it before authenticating again
- **`inject`** — what every operation running as this actor should carry

## Using an actor

Name it on the operation:

```yaml title="resources/products.yaml"
name: product
operations:
  create:
    method: POST
    path: /api/v1/vendors/{{vendor.vendor_id}}/products
    actor: vendor
    expect_status: 201
    extract:
      product_id: $.data.id
```

Two things happened there. `actor: vendor` means the `Authorization` header from
`inject` is attached. And `{{vendor.vendor_id}}` uses a value the *login response*
produced — extractions aren't limited to tokens. Anything the auth response
returns is available as `{{<actor>.<name>}}` anywhere in the schema.

Omit `actor:` for a public endpoint. No auth is attached and no login runs:

```yaml
  list:
    method: GET
    path: /api/v1/products
```

## Why actors are worth the trouble

Because a real workflow crosses identities. Approving a refund in the sample
project touches three:

```ansi
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create      (vendor)
  [2] Running: product.publish     (vendor)
  [3] Running: cart.add_item       (customer)
  [4] Running: order.create        (customer)
  [5] Running: order.pay           (customer)
  [6] Running: refund.request      (customer)
  [7] Running: refund.approve      (admin)
```

Three logins happen as part of that run, and **none of them appear as chain
steps** — a login is part of the step that needs it. You never asked for them,
and you never copied a token.

## Actors have their own login shapes

The `simple` strategy above is one request. Real systems vary, so there are
eleven strategies — OTP and MFA flows use `chain`, machine clients use
`oauth2_client_credentials`, some APIs need no call at all:

```yaml title="actors/service.yaml — no network call"
name: service
auth:
  strategy: bearer
  token: "{{secret.SERVICE_TOKEN}}"
```

See [auth strategies](/schema/auth-strategies/) for all eleven and their keys.

## One actor per identity, not per credential

The useful boundary is *who the API thinks you are*, not which token you hold.

Same credential, different acting identity — like Stripe's `Stripe-Account`
header — is still two actors, because the API treats them differently:

```yaml title="actors/platform.yaml"
name: platform
auth:
  strategy: bearer
  token: "{{secret.STRIPE_KEY}}"
inject:
  headers:
    Authorization: "Bearer {{platform.token}}"
```

```yaml title="actors/connected.yaml"
name: connected
auth:
  strategy: bearer
  token: "{{secret.STRIPE_KEY}}"
inject:
  headers:
    Authorization: "Bearer {{connected.token}}"
    Stripe-Account: "{{env.connected_account_id}}"
```

Conversely, don't create `admin_readonly` and `admin_write` if the API issues them
the same identity — that's one actor, and the difference belongs in the request.

## Sessions are cached

An actor authenticates once per session lifetime, not once per operation. Seven
steps as one customer means one login. See
[sessions & caching](/concepts/sessions/) for TTL, refresh, and expiry.

## Where credentials should live

Never in the schema. Environment variables hold the non-sensitive half and
`!secret` binds the rest to your OS keychain:

```yaml title="environments/local.yaml"
name: local
variables:
  vendor_email: vendor@marketplace.test
  vendor_password: !secret VENDOR_PASSWORD
```

The schema stays committable. See [secrets](/schema/secrets-and-transport/#secrets).

## Next

- [Auth strategies](/schema/auth-strategies/) — all eleven, with exact keys
- [Sessions & caching](/concepts/sessions/) — TTL and refresh
- [Resources & operations](/concepts/resources/) — what actors act on
