---
title: Common pitfalls
description: "The mistakes that pass lint and fail at run time — silently ignored keys, wrong-shaped blocks, and defaults that hide typos."
---

Reqloom's parser is deliberately forgiving: unknown keys are ignored so a newer
schema still loads on an older build. The cost is that **a typo is silently
dropped rather than reported.** Almost everything on this page lints clean.

The project below has six distinct mistakes in it:

```yaml
version: 1
name: SilentFailures
resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        expect_stats: 201            # 1. misspelled key
      typo_method:
        method: PSOT                 # 2. invalid method
        path: /orders
      assert_as_map:
        path: /orders
        assert:                      # 3. map instead of a sequence
          status_ok: "$.status_code == 200"
      depends_scalar:
        path: /orders
        depends_on: order.create     # 4. scalar instead of a sequence
      foreach_no_over:
        path: /orders
        for_each:                    # 5. no `over:`
          continue_on_error: true
```

```ansi
LINT OK — 0 actors, 1 resources, 6 operations. No errors.
```

Six mistakes, zero complaints. Here's each one.

## Misspelled keys are dropped

`expect_stats: 201` is not `expect_status: 201`. The key is unrecognised, so it's
discarded and the operation has no status expectation at all — meaning a `500`
response counts as success.

There is no strict mode and no unknown-key warning. When an operation behaves as
if you never configured something, **check the spelling first.** The keys that
get misspelled most: `expect_status`, `depends_on`, `query_params`, `body_form`,
`poll_until`, `pre_request`.

## Wrong values fall back silently

| Key | Bad value | Becomes |
| --- | --- | --- |
| `method` | `PSOT` | `GET` |
| `auth.strategy` | `oauth2` | `simple` |
| `auth.type` (inline) | `token` | no auth at all |
| `session.ttl` | `15 minutes` | `15m` |
| `connect_timeout` | `10 seconds` | `5s` |
| `extract` `source` | `json` | `jsonpath` |

`method: PSOT` becoming a `GET` is the nastiest of these — your POST silently
turns into a read, the request "succeeds", and nothing was created.

For `auth.strategy`, note that the valid value is
`oauth2_client_credentials`, not `oauth2`. Getting it wrong gives you the
`simple` strategy, which then tries to POST to an empty path.

## Blocks that must be sequences

`assert` and `depends_on` are **sequences**. Written any other way they're
ignored entirely:

```yaml
    # Ignored — assertions never run, step always passes
    assert:
      status_ok: "$.status_code == 200"

    # Correct
    assert:
      - expr: "$.status_code == 200"
        name: status_ok
```

```yaml
    depends_on: order.create        # ignored — no dependency created
    depends_on: [order.create]      # correct
```

If assertions never seem to fire, or a prerequisite never runs, check that the
lines start with `-`.

## `for_each` without `over`

`over:` is the only required key. Without it the whole block is dropped and the
operation runs once instead of fanning out:

```yaml
    for_each:
      over: product                 # required
      continue_on_error: true
```

## Lint validates the scope, not the field

This one deserves its own heading because it's the most common source of "it
lints but fails":

```yaml
      get:
        path: /orders/{{order.missing_var}}
```

That **passes** lint. The validator confirms a resource named `order` exists; it
does not check that anything extracts `missing_var`. You find out at run time:

```ansi
  FAIL   order.get (12ms) err=E_VAR_UNRESOLVED
```

Undefined *scopes* are caught, so a genuinely unknown name does fail:

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'order.get' references undefined symbol
'invoice.invoice_id': no actor, resource, env, or secret named 'invoice'
```

## Resource ids come from `name:`, not the filename

In a multi-file project, the id is the `name:` inside the file:

```yaml title="resources/orders.yaml"
name: order          # ← the id is `order`
operations:
  create: ...
```

So the operation is `order.create`, not `orders.create`, and references are
`{{order.order_id}}`. Mixing them up gives `E_REF_UNDEFINED` for a resource
you're certain exists. `reqloom lint` prints the counts it found — if a resource
is missing from the total, its file isn't being imported.

## Extractions belong to the resource

```yaml
    path: /orders/{{order.create.order_id}}/pay    # never resolves
    path: /orders/{{order.order_id}}/pay           # correct
```

There is no per-operation reference form. Everything an operation extracts is
stored under its **resource**, which is why a resource accumulates variables
across operations.

## `baseUrl`, not `base_url`

The magic environment variable is camelCase:

```yaml
environment:
  baseUrl: http://localhost:3000     # correct
  base_url: http://localhost:3000    # just an ordinary unused variable
```

With no `baseUrl`, requests are built from the path alone and fail in a way that
doesn't mention `baseUrl` at all.

## `{{env.X}}` is not `{{$.env.X}}`

- `{{env.NAME}}` — your schema's `environment:` block
- `{{$.env.NAME}}` — the **process** environment, via `getenv`

Both are valid syntax, so swapping them produces an unresolved variable rather
than an error. See [variable syntax](/reference/variables/#env-vs-env).

## A misspelled `--env` runs with no variables

```bash
reqloom run order.create --env stagng      # typo
```

This does **not** fail. The environment isn't found, so the run proceeds with
zero variables — every `{{env.X}}` unresolved, including `baseUrl`. Check the
`Env:` line in the summary when a run behaves strangely:

```ansi
Target: order.create   Env: stagng   Outcome: FAILED
```

## Polling needs the async status in `expect_status`

Polling only engages after the initial response matches `expect_status`. An
endpoint returning `202` then `200` needs both:

```yaml
    expect_status: [202, 200]        # correct
    expect_status: 200               # initial 202 fails before polling starts
```

Symptom is `E_STATUS_MISMATCH` on a step you thought was configured to poll.

## Milliseconds vs duration strings

Inconsistent, and worth memorising:

| Key | Format |
| --- | --- |
| `timeout` (operation) | bare integer ms — `30000` |
| `retry.backoff` | bare integer ms — `500` |
| `connect_timeout` | duration string — `5s`, `500ms` |
| `session.ttl` | duration string — `15m` |
| `poll_until.interval` / `timeout` | duration string — `2s`, `5m` |

`timeout: 30s` on an operation is not valid and falls back to the default.

## Quoted numbers stay strings in a body

Inside a structured `body:`, quoting controls the JSON type:

```yaml
    body:
      zip: "01234"        # → "01234"  (string, leading zero kept)
      zip2: 01234         # → 1234     (number, zero lost)
      flag: "true"        # → "true"   (string)
      flag2: true         # → true     (boolean)
```

Quote anything that is an identifier rather than a quantity — zip codes, phone
numbers, version strings, account numbers.

## Secrets are keychain-only

`!secret NAME` reads the OS keychain, never an environment variable. A missing
entry is not an error — the variable is left unresolved and you usually get a
401. And there is no CLI command to set one; see
[secrets](/schema/secrets-and-transport/#secrets).

## A quick checklist

When something doesn't work and lint is green:

1. Spelling of every key on the operation
2. `assert` / `depends_on` are sequences (`-` prefixed)
3. Resource id matches `name:`, not the filename
4. Extraction names actually exist upstream
5. `baseUrl` is set and spelled camelCase
6. The `Env:` line in the summary is the environment you meant
7. `expect_status` includes every status the endpoint can return
