---
title: Common pitfalls
description: "The mistakes that pass lint and fail at run time, the ones lint only warns about, and the ones the parser rejects with a file and line."
---

The parser rejects a wrong value when the project loads: a wrong type, an unknown
method or strategy, a malformed duration, a duplicate id. A misspelled **key** is
different. It is reported as a warning and ignored, and `reqloom lint` still
passes. This page covers both, and then the mistakes that lint clean and only
fail at run time.

```yaml
version: 1
name: Shop
resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        expect_stats: 201            # misspelled key
      pay:
        method: PSOT                 # unknown method
        path: /orders/pay
```

```ansi
LINT FAIL [E_SCHEMA_INVALID]: ./reqloom.yaml:11:17: unknown method 'PSOT'; accepted values: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS
```

Fix the method and lint passes, with a warning for the key:

```ansi
  WARNING: ./reqloom.yaml:9:9: unknown key 'expect_stats' in operation 'order.create'; ignored
LINT OK — 0 actors, 1 resources, 2 operations. No errors.
```

The load stops at the first error, and its warnings are only printed once the
project loads. Fix errors one at a time.

## Misspelled keys are warnings, not errors

`expect_stats: 201` is not `expect_status: 201`. The key is reported and
discarded, so the operation has no status expectation at all, and a `500`
response counts as success. `reqloom run` and the desktop app say nothing about
it; only `reqloom lint` prints the warning. Read lint's `WARNING:` lines, and treat them as
errors in CI if you want them to fail the build. The keys that get misspelled
most: `expect_status`, `depends_on`, `query_params`, `body_form`, `poll_until`,
`pre_request`.

## Wrong values fail the load

| Key | Bad value | Message |
| --- | --- | --- |
| `method` | `PSOT` | `unknown method 'PSOT'; accepted values: GET, POST, …` |
| `auth.strategy` | `oauth2` | `unknown strategy 'oauth2'; accepted values: simple, chain, …` |
| `auth.type` (inline) | `token` | `unknown type 'token'; accepted values: none, bearer, …` |
| `extract` `source` | `json` | `unknown source 'json'; accepted values: jsonpath, xpath, …` |
| `session.ttl` | `15 minutes` | `'ttl' must be a duration such as 500ms, 5s, 1m, 1h or 1d …` |
| `timeout` (operation) | `30s` | `'timeout' must be an integer, got '30s'` |
| `expect_status` | `abc` | `'expect_status' must be an integer, got 'abc'` |
| `assert` | a map | `'assert' must be a list, got a map` |
| `for_each` | no `over:` | `operation 'order.x' for_each needs 'over' naming a resource` |

Every message starts with `file:line:col`. `method` and the inline `auth.type`
match ignoring case (`Post` is `POST`); every other value is matched exactly.

For `auth.strategy`, the valid value is `oauth2_client_credentials` (or
`oauth2_password`), not `oauth2`. The inline `auth.type` accepts `oauth2`.

## `depends_on` as one item

`depends_on` takes a list or a single operation id. Both of these work:

```yaml
    depends_on: order.create
    depends_on: [order.create, user.login]
```

A map is an error.

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
LINT FAIL [E_REF_UNDEFINED]: ./reqloom.yaml:10: Operation 'order.get' references undefined symbol 'invoice.invoice_id': no actor, resource, env, or secret named 'invoice'
```

## A missing `imports:` ignores every file

The files are right there on disk, and nothing reads them. Given this project:

```
my-api/
├── reqloom.yaml          ← no `imports:` block
└── resources/
    └── orders.yaml
```

```ansi
LINT OK — 0 actors, 0 resources, 0 operations. No errors.
```

Exit code `0`, and no warning: without an `imports:` block there is nothing to
compare against. Multi-file projects **require** the globs:

```yaml title="reqloom.yaml"
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

With `imports:` present, lint warns about a pattern that matches no files — a
typo such as `resourses/*.yaml`, or `resources/**/*.yaml`, which is not expanded
and matches nothing — and about a file outside `actors/`, `resources/` and
`environments/`, which is ignored:

```ansi
  WARNING: ./reqloom.yaml:4:5: imports pattern 'resources/**/*.yaml' matched no files
  WARNING: ./reqloom.yaml:5:5: imported file ./other/x.yaml is not under actors/, resources/ or environments/; ignored
```

Always sanity-check the counts `lint` prints against what you expect.

## Resource ids come from `name:`, not the filename

In a multi-file project, the id is the `name:` inside the file:

```yaml title="resources/orders.yaml"
name: order          # ← the id is `order`
operations:
  create: ...
```

So the operation is `order.create`, not `orders.create`, and references are
`{{order.order_id}}`. Mixing them up gives `E_REF_UNDEFINED` for a resource
you're certain exists. Two files with the same `name:`, or a file and an inline
`resources:` entry with the same id, fail the load naming both files.

## Extractions belong to the resource

```yaml
    path: /orders/{{order.create.order_id}}/pay    # never resolves
    path: /orders/{{order.order_id}}/pay           # correct
```

There is no per-operation reference form. Everything an operation extracts is
stored under its **resource**, so `order.create`'s `order_id` and `order.pay`'s
`payment_id` are both read as `{{order.…}}`.

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
- `{{$.env.NAME}}` — the **process** environment, via `getenv`, and only for
  names listed under `allowed_env:` in `reqloom.yaml`

`{{$.env.NAME}}` for a name that isn't in `allowed_env` fails the load, in an
operation, an environment value or an actor. Writing `{{env.NAME}}` where you
meant the process variable lints clean and fails at run time with
`E_VAR_UNRESOLVED`. See [variable syntax](/reference/variables/#env-vs-env).

## A misspelled `--env` is refused

```bash
reqloom run order.create --env stagng      # typo
```

The run doesn't start. The message lists the environments the project has, and
the exit code is `2`:

```ansi
Error: unknown environment 'stagng'; the project has: local, staging
```

## Polling needs the async status in `expect_status`

Polling only engages after the initial response matches `expect_status`. An
endpoint returning `202` then `200` needs both:

```yaml
    expect_status: [202, 200]        # correct
    expect_status: 200               # initial 202 fails before polling starts
```

Symptom is `E_HTTP_4XX` (on a `202`) for a step you thought was configured to poll.

## Milliseconds vs duration strings

Inconsistent, and worth memorising:

| Key | Format | A bare number means |
| --- | --- | --- |
| `timeout` (operation) | integer ms — `30000` | milliseconds |
| `retry.backoff` | integer ms — `500` | milliseconds |
| `connect_timeout` | duration — `5s`, `500ms` | seconds |
| `session.ttl` | duration — `15m`, `1h` | seconds |
| `poll_until.interval` / `timeout` / `backoff` | duration — `2s`, `5m`, `500ms` | seconds |

A duration is a whole number with an optional `ms`, `s`, `m`, `h` or `d`. So
`ttl: 3600` is an hour and `connect_timeout: 30` is 30 seconds, but `timeout: 30`
on an operation is 30 milliseconds. `timeout: 30s` on an operation fails the
load, as does any other unit or spelling (`10 seconds`, `5x`). A `session.ttl`
is kept in whole seconds, so `900ms` counts as 0 seconds.

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

## A placeholder in an assertion becomes one value

`{{…}}` in an assertion or poll condition is substituted, as a value. Where you
put it decides the type:

```yaml
    assert:
      - "$.data.id == '{{order.order_id}}'"     # always a string comparison
      - "$.data.count == {{env.expected}}"      # a number if the value is one
```

Outside quotes, a value that is exactly a number, `true`, `false` or `null` is
used bare; anything else becomes a quoted string. So `{{env.expected}}` holding
`007` compares as a number, not as the string `007`. Put the placeholder inside quotes when you mean
a string. A reference in an assertion also makes its producer part of the chain,
like a reference in a path. See
[templates in predicates](/schema/advanced-operations/#templates-in-predicates).

## A form value starting with `@` is a file

In `body_form`, one leading `@` means "upload this file". To send text that
begins with `@` — a handle, a mention — double it:

```yaml
    body_form:
      avatar: "@./fixtures/avatar.png"     # file
      handle: "@@mirza"                   # the text @mirza
```

A relative path resolves against the project folder, not where you ran the
command, and `~` is not expanded.

## Retries don't re-send a POST

`retry.max` is the total number of sends, and POST and PATCH are re-sent only
when the request never reached the server (DNS or connect failure). A timeout,
or a 503 that `expect_status` doesn't allow, fails a POST on the first attempt.
And an operation without `expect_status` accepts every status, so a 5xx is never
retried on any method. If the endpoint is safe to
repeat, set `retry.non_idempotent: true`; see
[what is retried](/schema/secrets-and-transport/#what-is-retried).

## Don't escape values for JSON yourself

In a JSON body, a value substituted inside a string is escaped for you. A value
you already escaped by hand — `He said \"hi\"` — is escaped again and arrives with
the backslashes in it. Store the plain text. See
[JSON bodies](/reference/variables/#json-bodies).

## XPath extraction does nothing

`source: xpath` parses, but XML extraction is not implemented: the variable never
gets a value, and whatever references it fails with `E_VAR_UNRESOLVED`. Use
`source: regex` against the XML text for now.

## A `null` extraction doesn't fail the step

When an extract path finds JSON `null`, the step still passes. A later reference
to that field is unresolved and fails with `E_VAR_UNRESOLVED`, naming the cause:

```ansi
Variable: {{user.user_id}}
Location: …
Cause: The last user.create response returned null for 'user_id'.
```

It does not fall back to a value from an earlier run. To fail at the step that
returned `null`, add an assertion such as `$.data.id != null`.

## Secrets need a project id, and stay with their folder

`!secret NAME` reads the OS keychain, never an environment variable. Three things
catch people:

- **No `id:` in `reqloom.yaml`.** Secrets are stored per project, so a project
  that uses them needs an id. `reqloom lint` warns; a real run fails. The desktop
  app adds one when it opens the project.
- **A copied project.** The id is tied to the folder that first used it, so a copy
  gets `E_SECRET_ACCESS_FAILED`. Give the copy its own `id:` and enter its secrets
  again. A *moved* project is confirmed in the desktop app's Manage Secrets.
- **A missing entry.** Not a keychain error — the variable is left unresolved, so
  an operation fails with `E_VAR_UNRESOLVED` and a login usually gets a 401.

There is no CLI command to set a secret; see
[secrets](/schema/secrets-and-transport/#secrets).

## A quick checklist

When something doesn't work and lint is green:

1. Lint's `WARNING:` lines — each one is a key that was ignored
2. Resource id matches `name:`, not the filename
3. Extraction names actually exist upstream
4. `baseUrl` is set and spelled camelCase
5. The `Env:` line in the summary is the environment you meant
6. `expect_status` includes every status the endpoint can return
7. A placeholder in an assertion is quoted when you mean a string
8. A bare number in a duration field means seconds; in `timeout` and
   `retry.backoff` it means milliseconds
