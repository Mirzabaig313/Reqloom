---
title: Schema authoring guide
description: "How to write or review ChainAPI YAML schemas. Real patterns, real bugs, real fixes — grounded in production validation."
---

This guide is for anyone hand-writing a ChainAPI schema or reviewing
output from the AI importer. The patterns here come from real-world
validation against the GiGwala backend (174 endpoints, 5 actors).

## When to write by hand vs use the AI importer

Hand-write when:

- The API has fewer than ~5 endpoints
- Auth is unusual (HMAC signing, custom token formats, mTLS)
- You're authoring an example for documentation or a sample project
- You want full control over how operations are named

Use the [AI importer](/ai-importer/playbook/) when:

- You have OpenAPI / Postman / Markdown docs / curl logs to start from
- The API has 20+ endpoints with predictable patterns
- You'd rather review LLM output than draft from scratch

## Writing your first actor

The smallest useful schema has one actor and one operation:

```yaml
# chainapi.yaml
version: 1
name: My API
environment:
  baseUrl: http://localhost:3000

actors:
  user:
    auth:
      method: POST
      path: /login
      body:
        email: test@example.com
        password: hunter2
      extract:
        token: $.token
    inject:
      headers:
        Authorization: "Bearer {{user.token}}"

resources:
  hello:
    operations:
      get:
        method: GET
        path: /api/hello
        actor: user
```

Run it:

```bash
chainapi run hello.get
```

The engine logs in as `user`, captures the token, and calls `/api/hello`
with `Authorization: Bearer <token>`. Done.

## Writing a chain

Add a second operation that depends on the first:

```yaml
resources:
  order:
    operations:
      create:
        method: POST
        path: /api/orders
        actor: user
        body:
          item: "widget"
        extract:
          order_id: $.id

      get:
        method: GET
        path: /api/orders/{{order.order_id}}     # ← implicit dep
        actor: user
```

`chainapi run order.get` resolves the chain automatically:

1. `user.login` (cached if recent)
2. `order.create` (because `order.get`'s path references `{{order.order_id}}`)
3. `order.get`

You wrote no glue. The `{{order.order_id}}` reference is enough — the
engine traces it back to whichever operation produces `order_id` in
its `extract:` block.

## Cross-actor scenarios

When an operation by actor A needs data from an operation that actor
B (or no actor) created, use `depends_on:` and reference the **resource
extraction**, not an actor session var:

```yaml
admin_organization:
  operations:
    verify:
      method: PATCH
      path: /api/admin/orgs/{{auth.employer_org_id}}/verify   # ← resource ref
      actor: admin
      depends_on: [auth.register_employer]                    # ← explicit prereq
      body:
        status: verified
      expect_status: 200
```

The verify op is run as admin, but the `org_id` it needs comes from a
prior `auth.register_employer` call (no actor required). The path
references `{{auth.employer_org_id}}` — that's the **`auth` resource's**
extracted variable from the registration step.

If you wrote `{{employer.org_id}}` instead, the engine would try to look
up `org_id` in the **employer actor's session**, which doesn't exist
because the employer doesn't authenticate in this chain. The result
would be `E_VAR_UNRESOLVED`.

This is the single most common LLM-generated bug. The pattern to remember:

| If the value comes from... | Reference it as... |
|---|---|
| The currently-running actor's auth response | `{{<actor>.<field>}}` |
| Any operation's `extract:` block | `{{<resource>.<field>}}` |
| The local environment file | `{{env.<key>}}` |
| The OS keychain | `{{secret.<key>}}` (with `!secret` in the env file) |

## Per-run unique values

If the backend rejects duplicates (phone numbers, emails, idempotency
keys), don't hardcode them. Three patterns:

```yaml
# 1. Per-run override via --var flag:
body:
  phone: "{{env.new_user_phone}}"
```

```bash
chainapi run auth.register --var new_user_phone="+91999000123"
```

```yaml
# 2. Generate unique within the run:
body:
  idempotency_key: "{{$.uuid}}"
  email: "{{$.faker.email}}"
  name: "Test User {{$.uuid}}"
```

```yaml
# 3. From OS keychain (sensitive):
# environments/local.yaml
admin_password: !secret ADMIN_PASSWORD
```

## Where to next

- [File structure](/schema/file-structure/) — multi-file vs single-file layouts
- [Auth strategies](/schema/auth-strategies/) — simple, chain, OAuth, API key
- [Common pitfalls](/schema/pitfalls/) — every failure mode I hit during validation
- [Cheat sheet](/schema/cheatsheet/) — quick lookup table
