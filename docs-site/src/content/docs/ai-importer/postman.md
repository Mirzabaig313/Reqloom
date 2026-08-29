---
title: Importing Postman
description: "Postman collections import directly. The interesting part is converting collection variables and pre-request scripts into actors and dependencies."
---

Postman v2.1 collections import natively, so start there:

```bash
reqloom import my-collection.postman_collection.json --out my-api
```

```ansi
Imported 11 resources, 105 operations into my-api/reqloom.yaml
```

Collection folders become resources, requests become operations, and collection
variables become environment variables.

## What converts cleanly

| Postman | Becomes |
| --- | --- |
| Folder | A resource |
| Request | An operation |
| Method, URL, headers, body | The same fields |
| `{{variable}}` | `{{env.variable}}` |
| Collection variables | `environments/default.yaml` |

Postman's variable syntax is conveniently close, so `{{token}}` becomes
`{{env.token}}`:

```yaml title="environments/default.yaml"
name: default
variables:
  baseUrl: http://localhost:3000/api
  token: ""
```

Each operation carries provenance back to the request it came from:

```yaml
    _provenance:
      source: postman_import
      imported_at: 2026-08-29T08:15:47Z
      evidence:
        postman_name: Create Academic Year
```

Handy when you're reconciling 105 operations against the original collection.

## What doesn't convert

**Pre-request and test scripts.** A Postman collection encodes its workflow in
JavaScript — `pm.environment.set("order_id", pm.response.json().id)` in a test
script, read back as `{{order_id}}` in the next request. That's the dependency
graph, expressed imperatively, and it doesn't translate structurally.

This is the actual conversion work, and it's mechanical once you see the pattern:

```js
// Postman test script
pm.environment.set("order_id", pm.response.json().data.id);
```

```yaml
# Reqloom
    extract:
      order_id: $.data.id
```

Then the consumer changes from an environment variable to a resource reference:

```yaml
    path: /orders/{{order.order_id}}     # not {{env.order_id}}
    depends_on: [order.create]
```

That difference is the whole point. In Postman the value is global mutable state
set by whichever request ran last, so the collection only works if you run it in
order. In Reqloom the value belongs to the resource, so the engine can work out the
order itself.

**Auth.** Postman's auth helpers and a pasted bearer token both become an
`Authorization` header on each request. Replace them with an actor:

```yaml title="actors/user.yaml"
name: user
auth:
  strategy: simple
  method: POST
  path: /auth/login
  body:
    email: "{{env.user_email}}"
    password: "{{env.user_password}}"
  extract:
    token: $.data.accessToken
inject:
  headers:
    Authorization: "Bearer {{user.token}}"
```

Then drop the per-operation header and add `actor: user`.

## Suggested order of work

1. **Direct import** — get the mechanical translation for free
2. **Lint** — `reqloom lint --project my-api`
3. **Read the scripts** in the original collection. Every
   `pm.environment.set(...)` is an `extract`; every matching `{{var}}` read is a
   reference plus a `depends_on`
4. **One actor** to replace the token variable
5. **Wire only the chains you'll test** — not all 105 operations

If the collection is large and script-heavy, the
[AI importer](/ai-importer/playbook/) can propose the actor and dependency
structure from the scripts. Read the plan before accepting the YAML.

## Collection-runner ordering

A Postman collection's request order is significant — the runner executes
top-to-bottom and later requests depend on earlier side effects. That ordering is
an excellent source of truth for dependency edges. Read it as: *request N probably
depends on whichever earlier request set the variables it reads.*

You don't need to preserve the order itself. Once the edges are declared, the
resolver derives it.

## Things to check

| Check | Why |
| --- | --- |
| Literal IDs in paths | `/orders/1` should be a reference |
| Hardcoded tokens in headers | Should come from an actor |
| Disabled requests | Imported as normal operations |
| `expect_status` | Postman doesn't record expected status; the importer can't infer it |
| Duplicated requests across folders | Postman collections accumulate these |

Note the fourth: because a collection has no notion of an expected status, imported
operations have **no `expect_status`** — so any response, including a `500`, counts
as success until you add one.

## Next

- [`reqloom import`](/cli/import/) — the direct importer in full
- [Variables & references](/concepts/variables/) — resource refs vs global variables
- [AI importer](/ai-importer/playbook/) — for script-heavy collections
