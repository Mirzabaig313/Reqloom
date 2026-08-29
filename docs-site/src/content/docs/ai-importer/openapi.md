---
title: Importing OpenAPI
description: "Use the direct importer first — it needs no LLM. Then use the AI importer only for what a spec cannot express: actors and dependencies."
---

For OpenAPI you have two paths, and the cheap one is usually right.

:::tip[Try the direct importer first]
`reqloom import` handles OpenAPI 3.x natively — no LLM, no cost, deterministic
output. Run it before reaching for prompts.

```bash
reqloom import openapi.yaml --out my-api
```
:::

## What a spec gives you

An OpenAPI document describes paths, methods, parameters, bodies, and responses.
The direct importer translates all of it:

```bash
reqloom import openapi.yaml --out my-api
reqloom lint --project my-api
```

```ansi
Imported 11 resources, 105 operations into my-api/reqloom.yaml
LINT OK — 0 actors, 11 resources, 105 operations. No errors.
```

Note the **0 actors**. That's not a bug in the importer — it's what a spec is.

## What a spec cannot give you

Two things, and they're the two Reqloom is built around:

**Actors.** `securitySchemes` says *a bearer token is required*. It does not say
which endpoint issues one, what credentials it takes, or what the response looks
like. That's operational knowledge no spec carries.

**Dependency order.** A spec lists `POST /orders` and `POST /orders/{id}/pay` as
peers. Nothing states that the second needs the first, or that the `{id}` comes
from the first's response body. `{id}` is just a string parameter.

So the importer's output is a flat inventory. Turning it into workflows is the
work, and it's where the AI importer helps — or where you do it by hand for the
handful of chains you actually care about.

## Combining the two

The pragmatic route for a large spec:

1. **Direct import** for the mechanical translation — paths, methods, headers,
   bodies. Deterministic and free.
2. **The AI importer** pointed at the same spec, to *propose* actors and
   dependency edges. Read the plan it produces; it's five minutes, and it's where
   the guessing is visible.
3. **Hand-wire** the chains you'll test. Usually a handful of `depends_on` lines
   and one actor.

That way the LLM never touches the parts a spec states precisely, and only advises
on the parts it doesn't.

## Wiring an actor

Find the login endpoint in the imported output and turn it into an actor:

```yaml title="actors/user.yaml"
name: user
auth:
  strategy: simple
  method: POST
  path: /auth/login
  body:
    email: "{{env.user_email}}"
    password: "{{env.user_password}}"
  expect_status: 200
  extract:
    token: $.data.accessToken
inject:
  headers:
    Authorization: "Bearer {{user.token}}"
```

Then delete the per-operation `Authorization` header the importer generated and add
`actor: user`. One edit per operation, and the token is never pasted anywhere.

## Wiring dependencies

Imported paths keep literal values from the spec's examples:

```yaml
  get_order:
    method: GET
    path: /orders/1              # ← literal from the spec
```

Replace the literal with a reference, which creates the dependency implicitly:

```yaml
  get_order:
    method: GET
    path: /orders/{{order.order_id}}
    depends_on: [order.create]
```

## Things to check in imported output

| Check | Why |
| --- | --- |
| `expect_status` matches reality | Specs often document `200` where the API returns `201` |
| Required vs optional body fields | Importers include documented examples, not minimal valid bodies |
| Literal path IDs | `/orders/1` should become a reference |
| Enum values | A spec's first enum value isn't necessarily valid for your data |
| `$ref` composition | Deeply composed schemas may flatten in surprising ways |

Specs drift from implementations. Treat imported `expect_status` and bodies as
claims to verify on the first real run, not facts.

## When the spec is unreliable

If the spec is generated but stale — common with hand-maintained OpenAPI — the
direct importer will faithfully reproduce its errors. In that case a curl log of
real traffic is better input, even though it's messier. See
[importing curl logs](/ai-importer/curl/).

## Next

- [`reqloom import`](/cli/import/) — the direct importer in full
- [AI importer](/ai-importer/playbook/) — the prompt and the workflow
- [Dependency resolution](/concepts/dependencies/) — what you're wiring toward
