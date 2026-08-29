---
title: Cheat sheet
description: "One-line answers for the things you look up repeatedly: keys, references, flags, and the silent-failure gotchas."
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
| Keep a password out of the repo | `password: !secret MY_PASSWORD` |
| Read a CI environment variable | `{{$.env.CI_TOKEN}}` |
| Make an operation public (no auth) | Omit `actor:` |
| Declare a prerequisite that returns no value | `depends_on: [other.op]` |
| Send a form body | `body_form:` instead of `body:` |
| Upload a file | `body_form: { field: "@./path.png" }` |
| Accept more than one status | `expect_status: [200, 204]` |
| Check the response body | `assert:` — must be a **sequence** |
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
        depends_on: [cart.add_item]      # must be a sequence
        extract:
          order_id: $.data.id
        assert:
          - "$.data.total > 0"
        timeout: 30000            # milliseconds, bare integer
        retry: { max: 3, backoff: 500 }  # backoff in ms
        force: false
        pre_request: ./hooks/sign.js
        post_response: ./hooks/log.js
```

## Extraction sources

| Path shape | Reads |
| --- | --- |
| `$.data.id` | JSON body (default) |
| `$.headers.Location` | A response header |
| `$.cookies.session` | A cookie |
| `$.status_code` | The status code |
| `$.data[*].id` | A list → one resource instance per match |

XPath and regex need the explicit form:

```yaml
    extract:
      title:
        path: "//book/title"
        source: xpath
      order_ref:
        path: "ref-([0-9]+)"
        source: regex
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
reqloom lint [--project P]
reqloom import <spec> [--out D] [--project-root D] [--force]
```

Exit codes: `0` pass · `1` failure · `2` bad arguments · `3` crash.

The positional argument comes **first** — `reqloom run order.create --env staging`,
never `reqloom run --env staging order.create`.

## Silent failures to watch for

These produce no error. They are the most common reason a schema "doesn't work"
while linting clean:

| Mistake | What happens |
| --- | --- |
| Misspelled key (`expect_stats:`) | Ignored entirely |
| Unknown `method:` | Becomes `GET` |
| Unknown `auth.strategy` | Becomes `simple` |
| `assert:` written as a map | Ignored — assertions never run |
| `depends_on:` written as a scalar | Ignored — no dependency |
| `for_each:` without `over:` | Whole block dropped |
| Malformed `session.ttl` | Becomes 15m |
| Malformed `connect_timeout` | Becomes 5s |
| `--env` naming a missing environment | Runs with zero variables |
| Typo in an extracted name | Passes lint, fails at run with `E_VAR_UNRESOLVED` |

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
| `lint --format` / `lint --help` | Text output only; `reqloom --help` |
