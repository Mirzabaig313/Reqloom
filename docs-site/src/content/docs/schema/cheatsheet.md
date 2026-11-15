---
title: Cheat sheet
description: "One-line answers for the things you look up repeatedly: keys, references, flags, and the gotchas that lint misses."
---

Everything on this page is verified against the parser. If a key isn't here, it
probably isn't real — see [what doesn't exist](#things-that-do-not-exist).

## I want to…

| Goal | Do this |
| --- | --- |
| Reference an ID from an earlier response | `{{<resource>.<extracted_name>}}` |
| Reference a specific one of many | `{{<resource>[1].<name>}}` — 1-indexed |
| Reference an actor's token | `{{<actor>.<var>}}`, usually in `inject.headers` |
| Set the API host | `baseUrl` in the environment — camelCase, it's magic |
| Generate a unique value per request | `{{$.uuid}}` |
| Generate a timestamp | `{{$.now}}`, or `{{$.now + 24h}}` |
| Keep a password out of the repo | `password: !secret MY_PASSWORD`, plus `id:` in `reqloom.yaml` |
| Read a CI environment variable | `{{$.env.CI_TOKEN}}`, with `CI_TOKEN` listed in `allowed_env:` |
| Make an operation public (no auth) | Omit `actor:` |
| Declare a prerequisite that returns no value | `depends_on: [other.op]` |
| Send a form body | `body_form:` instead of `body:` |
| Upload a file | `body_form: { field: "@./path.png" }` — relative to the project folder |
| Send form text starting with `@` | `body_form: { handle: "@@mirza" }` |
| Retry a POST that is safe to repeat | `retry: { max: 3, non_idempotent: true }` |
| Accept more than one status | `expect_status: [200, 204]` |
| Check the response body | `assert:` — must be a **sequence** |
| Compare with an earlier value | `assert: ["$.id == '{{order.order_id}}'"]` |
| Choose what a failure message quotes | `failure_message: { show: [$.error.message] }` |
| Raise the response size limit | `transport: { max_response_size: 256MiB }` |
| Poll an async endpoint | `poll_until:` + `expect_status: [202, 200]` |
| Run once per item in a list | extract with `$.data[*].id`, then `for_each: { over: <res> }` |
| Force a step to re-run | `force: true` |
| Validate without sending requests | `reqloom lint` |
| Run against staging | `reqloom run <op> --env staging` |
| Override one variable for one run | `reqloom run <op> --var key=value` |
| Get machine-readable results | `--format json` or `--format junit` |
| Write results to a file | `--output results.xml` |

## Operation keys

```yaml
resources:
  order:                          # resource id — or `name:` in its own file
    operations:
      create:                     # → operation id `order.create`
        method: POST              # default GET
        path: /api/v1/orders      # appended to baseUrl
        actor: customer           # who runs it
        headers: { X-Foo: bar }
        query_params: { limit: "20" }    # NOT `query:` or `params:`
        body:                     # map/list → JSON; scalar → verbatim
          total: 100
        body_form: { a: "1" }     # form-encoded or multipart
        expect_status: 201        # or [201, 200]
        depends_on: [cart.add_item]      # or a single id
        extract:
          order_id: $.data.id
        assert:
          - "$.data.total > 0"
        timeout: 30000            # milliseconds, bare integer
        retry: { max: 3, backoff: 500 }  # max = total sends; backoff in ms
        force: false
        pre_request: ./hooks/sign.js
        post_response: ./hooks/log.js
        failure_message: { show: [$.error.message] }
```

## Extraction sources

| Path shape | Reads |
| --- | --- |
| `$.data.id` | JSON body (default) |
| `$.headers.Location` | A response header |
| `$.cookies.session` | A cookie |
| `$.status_code` | The status code |
| `$.data[*].id` | A list → one resource instance per match |

Regex needs the explicit form:

```yaml
    extract:
      order_ref:
        path: "ref-([0-9]+)"
        source: regex
```

`$..name` is not supported, and `source: xpath` parses but is not implemented.

## Project file

```yaml title="reqloom.yaml"
version: 1
name: My API
id: 3f6b2a9e-1c4d-4e8a-9b7f-2d5c8e1a4b60   # required once you use secrets
default_environment: local
allowed_env: [CI_TOKEN]                    # what {{$.env.X}} may read
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
```

## Auth strategies

`simple` `chain` `basic` `api_key` `oauth2_client_credentials`
`oauth2_password` `oauth1` `aws_sigv4` `bearer` `jwt` `mtls`

```yaml title="actors/vendor.yaml"
name: vendor
auth:
  strategy: simple
  method: POST
  path: /api/v1/auth/login
  body:
    email: "{{env.vendor_email}}"
    password: "{{env.vendor_password}}"
  expect_status: 200
  extract:
    token: $.data.accessToken
session:
  ttl: 15m
inject:
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

See [auth strategies](/schema/auth-strategies/) for the keys each one takes.

## CLI

```bash
reqloom run <resource.operation> [--project P] [--env E] [--var K=V]
                                 [--format text|json|junit] [--output F] [--quiet]
                                 [--failure-body-size SIZE]
reqloom lint [--project P]
reqloom import <spec> [--out D] [--project-root D] [--force] [--no-secrets]
```

Exit codes: `0` pass · `1` failure · `2` bad arguments · `3` crash.

The positional argument comes **first** — `reqloom run order.create --env staging`,
never `reqloom run --env staging order.create`.

## Mistakes lint lets through

These pass `reqloom lint`, some with a warning. They are the most common reason a
schema "doesn't work" while linting clean:

| Mistake | What happens |
| --- | --- |
| Missing `imports:` in a multi-file project | Every file ignored; 0 resources, exit 0, no warning |
| `imports:` pattern that matches nothing | Warning; lint passes |
| File outside `actors/` `resources/` `environments/` | Warning; the file is ignored |
| Misspelled key (`expect_stats:`) | Warning; the key is ignored |
| `--env` naming a missing environment | Refused, exit code `2`; the message lists the environments |
| Typo in an extracted name | Passes lint, fails at run with `E_VAR_UNRESOLVED` |
| No `expect_status` | Every status passes, a `500` included |
| An extract path that finds `null` | Step passes; a later reference is unresolved |

And these fail loudly, at load, with the file and line: a wrong type (`timeout:
30s`), an unknown `method`, `auth.strategy`, `auth.type` or extraction `source`,
a malformed duration, `assert:` written as a map, `for_each:` without `over:`, an
id defined twice or containing `/`, `\` or `:`, an unknown `actor` or `for_each`
resource, a malformed assertion, an unknown `$.faker` name, a `$.env` name not in
`allowed_env`, a refused regex, an unsupported `failure_message.show` path, and
an ambiguous producer.

Durations: a bare number is **seconds** in `ttl`, `connect_timeout`, and
`poll_until` fields, but **milliseconds** in `timeout` and `retry.backoff`.

Full detail in [common pitfalls](/schema/pitfalls/).

## Things that do not exist

Frequently assumed, never implemented:

| Not real | Use instead |
| --- | --- |
| `--dry-run` on `run` | `reqloom lint` — it dry-runs every operation |
| `--version` | Not implemented; check the release you downloaded |
| `url:` on an operation | `baseUrl` + `path:` |
| `query:` / `params:` | `query_params:` |
| `environments:` (plural) root key | `environment:` plus `imports:` of `environments/*.yaml` |
| `secrets:` block | The `!secret` tag on an environment variable |
| `$.random`, `$.timestamp`, `$.date` | `{{$.uuid}}`, `{{$.now}}` |
| `{{$var}}` | `{{$.var}}` — the dot is required |
| Resource-level `base_path` / `headers` / `actor` | Repeat per operation |
| `for_each` alias or `limit` | Only `over` and `continue_on_error` |
| `retry.max_backoff` | Fixed at 30s |
| Duration strings for `timeout` / `retry.backoff` | Bare milliseconds |
