---
title: Schema authoring guide
description: "Build a Reqloom schema from nothing, one addition at a time — first actor, first chain, multi-file split, and the reference bug that catches everyone."
---

We'll start with the smallest schema that runs and grow it. Every snippet is
valid on its own, so you can follow along and lint after each step.

If you already have an OpenAPI spec or a Postman collection, don't hand-write —
[`reqloom import`](/cli/import/) converts those directly. Hand-writing is right for
a small API, an unusual auth flow, or when you want control over naming.

## 1. The smallest schema that runs

One file, one actor, one operation:

```yaml title="reqloom.yaml"
version: 1
name: My API
default_environment: local

environment:
  baseUrl: http://localhost:3000
  user_email: test@example.com
  user_password: !secret USER_PASSWORD

actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /login
      body:
        email: "{{env.user_email}}"
        password: "{{env.user_password}}"
      expect_status: 200
      extract:
        token: $.token
    inject:
      headers:
        Authorization: "Bearer {{user.token}}"

resources:
  profile:
    operations:
      get:
        method: GET
        path: /api/me
        actor: user
        expect_status: 200
```

Check it parses before you run anything:

```bash
reqloom lint
```

```ansi
LINT OK — 1 actors, 1 resources, 1 operations. No errors.
```

Then run it:

```bash
reqloom run profile.get
```

The engine logs in as `user`, keeps the token, and calls `/api/me` with the
`Authorization` header. The login is not a step — it belongs to the step that
needed it.

Three things worth noticing:

- **`baseUrl` is magic.** The request URL is `baseUrl` + `path`. It's camelCase,
  and it stops at the host — no `/api` prefix.
- **The password is `!secret`.** It reads from your OS keychain, so this file is
  safe to commit. See [secrets](/schema/secrets-and-transport/#secrets).
- **`expect_status` is worth setting.** Without it, a `500` counts as success.

## 2. Your first chain

Add an operation that needs the first one:

```yaml
resources:
  order:
    operations:
      create:
        method: POST
        path: /api/orders
        actor: user
        body:
          item: widget
          quantity: 2
        expect_status: 201
        extract:
          order_id: $.data.id

      get:
        method: GET
        path: /api/orders/{{order.order_id}}   # ← this creates the dependency
        actor: user
        expect_status: 200
```

```bash
reqloom run order.get
```

```ansi
Running: order.get (chain of 2 steps, env=local)
  [1] Running: order.create (attempt 1)
  [2] Running: order.get (attempt 1)
```

You wrote no ordering. `{{order.order_id}}` is enough — the engine traces it to
whichever operation extracts `order_id` and runs that first.

### When a reference isn't enough

Some prerequisites return nothing you read but still have to happen. Declare those
with `depends_on`:

```yaml
      publish:
        method: POST
        path: /api/orders/{{order.order_id}}/publish
        actor: user
        depends_on: [order.create]
        expect_status: 200
```

Rule of thumb: if you use a value from it, the reference is the dependency. If you
only need its *side effect* — published, activated, verified, approved — say
`depends_on`.

`depends_on` must be a **sequence**. Written as a bare scalar it's silently
ignored and you get no dependency at all.

## 3. The bug that catches everyone

When an operation run by one actor needs a value produced by a *different*
actor's request, reference the **resource**, not the actor.

```yaml
  admin_org:
    operations:
      verify:
        method: PATCH
        path: /api/admin/orgs/{{signup.org_id}}/verify   # ← resource reference
        actor: admin
        depends_on: [signup.register_employer]
        body:
          status: verified
        expect_status: 200
```

`verify` runs as `admin`, but `org_id` came from a `signup.register_employer` call
that needed no actor at all. So it's `{{signup.org_id}}` — the **signup resource's**
extracted value.

Write `{{employer.org_id}}` instead and the engine looks for `org_id` in the
*employer actor's session*, which doesn't exist because the employer never
authenticated in this chain. You get `E_VAR_UNRESOLVED` at run time — and
critically, **lint won't catch it**, because lint validates that the scope exists,
not the field.

| If the value came from | Reference it as |
| --- | --- |
| The login response of the actor running the request | `{{<actor>.<field>}}` |
| Any operation's `extract:` block | `{{<resource>.<field>}}` |
| The environment file | `{{env.<key>}}` |
| The OS keychain | `{{secret.<key>}}` |
| Generated per request | `{{$.uuid}}`, `{{$.now}}`, `{{$.faker.email}}` |

Remember too that values belong to the **resource**, not the operation:
`{{order.order_id}}`, never `{{order.create.order_id}}`.

## 4. Values that must be unique

If the API rejects duplicates — emails, phone numbers, idempotency keys — don't
hardcode. Pick based on who needs to know the value:

```yaml
      register:
        method: POST
        path: /api/signup
        body:
          # Unique per request, nobody needs to know it
          email: "{{$.faker.email}}"
          idempotency_key: "{{$.uuid}}"
          name: "Test User {{$.uuid}}"
          # Supplied from outside, because you'll verify it later
          phone: "{{env.new_user_phone}}"
        expect_status: 201
```

```bash
reqloom run signup.register --var new_user_phone="+15555550123"
```

Use a builtin when you need *something* unique and don't care what. Use an
environment variable with `--var` when the caller has to know the value.

## 5. Assert more than the status code

`expect_status` checks the status. `assert` checks the body:

```yaml
      create:
        method: POST
        path: /api/orders
        actor: user
        expect_status: 201
        extract:
          order_id: $.data.id
        assert:
          - "$.data.status == 'pending'"
          - expr: "$.data.total > 0"
            name: total is positive
          - "$.data.currency in ['USD', 'EUR']"
```

`assert` must be a **sequence**. Written as a map it is silently ignored and the
step always passes — so if your assertions never seem to fire, check for the `-`.

## 6. Split the file once it grows

Past about five resources, one file gets unwieldy. Move to the multi-file layout:

```
my-api/
├── reqloom.yaml
├── environments/
│   ├── local.yaml
│   └── staging.yaml
├── actors/
│   ├── user.yaml
│   └── admin.yaml
└── resources/
    ├── orders.yaml
    └── profile.yaml
```

```yaml title="reqloom.yaml"
version: 1
name: My API
default_environment: local
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

```yaml title="resources/orders.yaml"
name: order            # ← the id. Operations become order.create, order.get
description: Customer orders
operations:
  create:
    method: POST
    path: /api/orders
    actor: user
    expect_status: 201
    extract:
      order_id: $.data.id
```

Two traps here, both silent:

- **`imports:` is what loads the files.** Without it, `resources/orders.yaml`
  is never read and you get `0 resources` with exit code 0.
- **The id comes from `name:`**, not the filename. `resources/orders.yaml` with
  `name: order` gives `order.create` and `{{order.order_id}}`.

Only `actors/`, `resources/`, and `environments/` are recognised directories, and
globs don't recurse. Details in [file structure](/schema/file-structure/).

## 7. Multiple environments

Each environment is a file, and its name is what `--env` selects:

```yaml title="environments/staging.yaml"
name: staging
variables:
  baseUrl: https://staging.example.com
  user_email: qa@example.com
  user_password: !secret STAGING_USER_PASSWORD
transport:
  connect_timeout: 10s
```

```bash
reqloom run order.get --env staging
```

A `--env` name that doesn't match any environment does **not** error — the run
proceeds with zero variables, so every `{{env.X}}` is unresolved. Check the `Env:`
line in the summary if a run behaves strangely.

## Verify as you go

```bash
reqloom lint                      # after every edit — fast, no network
reqloom run <op>                  # the only real proof
reqloom run <op> --format json    # for scripts and CI
```

Lint catches YAML errors, undefined scopes, missing `depends_on` targets, and
cycles. It cannot tell you a path, body, or status code is wrong — only a real
request can.

Sanity-check the counts it prints. `0 resources` when you have six files means
they aren't being imported.

## Habits worth keeping

- **Set `expect_status` on everything.** The default accepts any response.
- **Extract an id at login** so paths can use `{{actor.user_id}}` without an extra
  request.
- **Add a `refresh` block** to actors whose tokens expire, so an expiry costs one
  request rather than a full re-login.
- **Never hardcode a credential.** `!secret` for everything sensitive.
- **Quote identifier-shaped values.** `"01234"` stays a string; `01234` becomes
  the number `1234`.
- **Keep chains short.** A thirty-step chain usually means you're creating
  fixtures that could be seeded in the environment instead.

## Next

- [File structure](/schema/file-structure/) — layouts and import rules
- [Auth strategies](/schema/auth-strategies/) — all eleven, with their keys
- [Advanced operations](/schema/advanced-operations/) — polling, fan-out, hooks, uploads
- [Common pitfalls](/schema/pitfalls/) — the silent failures, in one place
- [Cheat sheet](/schema/cheatsheet/) — quick lookup
