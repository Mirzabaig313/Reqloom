---
title: What is Reqloom?
description: "A workflow-aware API testing tool. You declare what each endpoint needs; it resolves and runs the whole chain. Built for multi-actor SaaS APIs."
---

Reqloom treats your API as a graph of resources, actors, and dependencies rather
than a flat list of requests. You declare what each endpoint needs once; running
any endpoint then resolves and executes everything it depends on.

## The problem

Modern SaaS backends are multi-actor. To test a single endpoint you often have to:

1. Log in as several different identities
2. Create three to six prerequisite records
3. Copy IDs and bearer tokens between tabs
4. Redo all of it when tokens expire or the test database resets

Take *admin approves a customer refund* on a marketplace API. Before the request
you care about, six others must succeed — a vendor creates and publishes a
product, a customer adds it to a cart, orders it, pays, and requests a refund.
Three identities, seven requests, six IDs to carry forward. For one endpoint.

Across a twenty-endpoint feature that's hours of mechanical work per cycle, and
it's work you redo rather than keep.

## What you write instead

Each actor is declared once — how to log in, and what to attach to its requests:

```yaml title="actors/vendor.yaml"
name: vendor
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

Each operation states who runs it and what it needs:

```yaml title="resources/refunds.yaml"
name: refund
operations:
  approve:
    method: POST
    path: /api/v1/admin/refunds/{{refund.refund_id}}/approve
    actor: admin
    depends_on: [refund.request]
    expect_status: 200
```

That's the entire declaration for the endpoint above — one dependency, one actor.

## What you get

```bash
reqloom run refund.approve
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

Result: SUCCEEDED
```

Seven steps from one command. The three logins happen automatically and don't even
appear as steps — a login belongs to whichever step needs it, and sessions are
cached, so three identities cost three logins rather than seven.

Nobody wrote that ordering. It's derived two ways:

- **Implicitly**, from references. `{{refund.refund_id}}` in the path means
  "whatever produces this must run first".
- **Explicitly**, via `depends_on`, for prerequisites that return nothing you read
  but still have to happen — publishing a product, verifying an account.

Change the schema and the chain re-derives itself.

## The four ideas

| Concept | Is |
| --- | --- |
| [Actor](/concepts/actors/) | An identity — how to log in, what to attach to every request |
| [Resource](/concepts/resources/) | A thing your API manages, grouping operations and owning extracted values |
| [Dependency](/concepts/dependencies/) | What must happen first, declared not scripted |
| [Variable](/concepts/variables/) | A value extracted from one response and referenced in a later request |

That's the whole model. Everything else — polling, assertions, retries, hooks — is
detail on top.

## Who it's for

- **Backend engineers** testing endpoints during development
- **QA engineers** running regression flows across admin, web, and mobile roles
- **Anyone in CI** who wants a workflow test that fails with a real error code

## What it isn't

- **A replacement for an HTTP client** when you're poking at one request. If your
  workflow is one request at a time, Postman or Bruno is the right tool and
  Reqloom is overhead. It pays off once a request has prerequisites.
- **A load testing tool.** Use k6 or Gatling.
- **A collaboration platform.** Git is the sharing model — the schema is plain
  YAML that diffs and merges.

## How it compares

Postman, Bruno, and Insomnia are excellent HTTP clients. They model an API as a
list of requests, so anything about *order* lives in hand-written scripts —
`pm.environment.set(...)` in one request, read back in the next. That works, and it
stops working when someone runs the requests out of order or the collection grows
past what one person maintains.

Hurl and Stepci model workflows properly, but linearly: you author a full script
per scenario, and twenty endpoints means twenty scripts with duplicated setup.

Reqloom's difference is that dependencies are **declared on the endpoint**, not
scripted per scenario. Declare `refund.approve` needs `refund.request` once, and
every chain that passes through it gets the ordering for free.

Full breakdown on the [landing page](/).

## Next

- [Installation](/start/install/) — download a build, or compile from source
- [5-minute tour](/start/tour/) — watch the chain above resolve on the sample
- [The mental model](/concepts/mental-model/) — the four ideas in depth
- [Authoring guide](/schema/authoring/) — write your first schema
