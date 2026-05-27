---
title: The mental model
description: "Three concepts. Sixty seconds. The whole product fits in your head."
---

ChainAPI treats your API as a graph. Three concepts make up the model:

## Actors

An **actor** is an identity with its own auth flow. `admin`, `vendor`,
`customer`, `worker`. Each has:

- A login or token-acquisition flow (one or more HTTP requests)
- A session cache (token + extracted variables)
- A set of headers injected into every operation that uses this actor

```yaml
admin:
  auth:
    method: POST
    path: /api/v1/auth/login
    body: { email: "{{env.admin_email}}", password: "{{env.admin_password}}" }
    extract: { token: $.data.accessToken }
  inject:
    headers:
      Authorization: "Bearer {{admin.token}}"
```

When you run an operation that uses `actor: admin`, the engine
authenticates as admin (or reuses a cached session) before sending
the request.

## Resources

A **resource** is a domain entity. `order`, `payment`, `refund`. Each
holds a set of named **operations** — individual HTTP endpoints.

```yaml
order:
  operations:
    create:
      method: POST
      path: /api/v1/orders
      actor: customer
      body: { product_id: "{{product.product_id}}" }
      extract: { order_id: $.data.id }

    pay:
      method: POST
      path: /api/v1/orders/{{order.order_id}}/pay
      actor: customer
      body: { method: "card", token: "tok_test_visa" }
```

Operations capture HTTP method, path, headers, body, expected status,
which actor to run as, what to extract from the response, and what
prerequisites they depend on.

## Dependency chains

When you ask to run `order.pay`, the engine builds the **dependency
chain** automatically:

1. **Implicit edges**: any `{{X.y}}` reference in a path/body/header
   implies a dependency on whichever operation produces `X.y`.
   `{{order.order_id}}` in the path implies a dep on `order.create`.
2. **Explicit edges**: `depends_on:` declarations for prerequisites
   that don't produce values (e.g. `send_otp` before `verify_otp`).
3. **Topological sort** with deterministic tie-break (lexicographic).
4. **Session resolution**: every actor used in the chain must have a
   live session — the engine logs them in if needed (or reuses cached
   ones).
5. **Execution**: steps run in order, each with all prerequisites'
   outputs already in scope.

The chain for `order.pay` looks like:

```
customer.login                ← cached session, skipped if live
  → order.create              ← produces order_id
  → order.pay                 ← target
```

The chain for `refund.approve` is deeper:

```
admin.login
  → vendor.login
  → customer.send_otp
  → customer.verify_otp
  → product.create            (vendor)
  → product.publish           (vendor)
  → order.create              (customer)
  → order.pay                 (customer)
  → refund.request            (customer)
  → refund.approve            (admin)  ← target
```

You wrote zero glue code for this. The engine read your schema and built it.

## What this gets you

- **One-click testing** of any endpoint, regardless of dependency depth
- **First-class actors** — you don't re-author auth in every request
- **Self-documenting workflows** — the YAML schema doubles as system
  documentation
- **Reusable across environments** — same schema runs against local,
  staging, and production with different env vars
- **CI/CD-friendly** — the CLI is a drop-in test runner with JUnit
  output

## What this is NOT

- A scripting platform — the schema is declarative, hooks are an
  escape hatch
- A flow visualizer like Postman Flows — the visual is generated from
  the schema, not the other way around
- A GUI-first tool — the schema is the source of truth; the desktop app
  is a view onto it

## Next steps

- [Actors](/concepts/actors/)
- [Resources & operations](/concepts/resources/)
- [Dependency resolution](/concepts/dependencies/)
- [Variables & references](/concepts/variables/)
- [Sessions & caching](/concepts/sessions/)
