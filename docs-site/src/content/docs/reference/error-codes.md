---
title: Error codes
description: "Every E_* code the engine produces, grouped by class, with the usual cause and fix. Includes which codes are retried automatically."
---

The engine emits a stable `E_*` code for every failure. Codes are part of the
contract — the CLI, desktop, and JSON/JUnit output all report the same string,
and you can assert on them.

Where a code shows up:

```ansi
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
```

```json
{ "op": "product.create", "status": "FAIL", "error_code": "E_SESSION_REFRESH_FAILED" }
```

:::note[The exit code does not encode the error]
Every failure above is exit code `1`, whatever the class. Read the `E_*` code
from the output, not from `$?`. See [exit codes](/cli/overview/#exit-codes).
:::

## Schema

Raised at load time, before any request. `reqloom lint` catches all of these.

| Code | Cause | Fix |
| --- | --- | --- |
| `E_YAML_PARSE` | Malformed YAML, or a file over the 8 MiB cap | Check indentation; the message names the file and line |
| `E_SCHEMA_INVALID` | Structurally valid YAML the engine can't accept — commonly a bad hook path | Read the detail; it names the offending field |
| `E_SCHEMA_VERSION` | `version:` is missing or outside 1–3 | Set a supported `version:` |
| `E_REF_UNDEFINED` | A `{{X.y}}` scope, or a `depends_on` target, doesn't exist | Fix the name — the message names the symbol |
| `E_CYCLE` | Circular dependency | Break the loop; the message prints the path |

```ansi
LINT FAIL [E_CYCLE]: Circular dependency detected: order.one → order.two
```

## Resolution

Raised while building a request.

| Code | Cause | Fix |
| --- | --- | --- |
| `E_VAR_UNRESOLVED` | A reference couldn't be substituted | Most often a typo'd extraction name, or a missing env var |
| `E_UPLOAD_FILE_UNREADABLE` | A `body_form` `@path` is missing, not a regular file, or over 50 MiB | Check the path is relative to where you run the command |
| `E_INDEXED_REF_OUT_OF_RANGE` | **Reserved — never raised.** An out-of-range `{{res[N].x}}` surfaces as `E_VAR_UNRESOLVED` | — |

`E_VAR_UNRESOLVED` is the one you'll meet most. Remember that
[lint validates the scope, not the field](/cli/lint/#what-it-does-not-check) —
`{{order.typo}}` passes lint whenever a resource named `order` exists, then
fails here.

## Network

| Code | Cause | Retried |
| --- | --- | --- |
| `E_NETWORK_TIMEOUT` | Connect or read timeout | yes |
| `E_NETWORK_DNS` | Hostname didn't resolve | yes |
| `E_NETWORK_TLS` | TLS handshake failed | no |

`E_NETWORK_TLS` against an internal host usually means a private CA — point
`transport.ca_bundle` at it rather than disabling verification. See
[TLS settings](/schema/secrets-and-transport/#transport).

## HTTP

| Code | Cause | Retried |
| --- | --- | --- |
| `E_HTTP_5XX` | Server error | yes |
| `E_HTTP_4XX` | Client error | no |
| `E_STATUS_MISMATCH` | Status didn't match `expect_status` | no |

`E_STATUS_MISMATCH` with a `202` is the classic polling mistake: an async
endpoint needs `expect_status: [202, 200]`, not `expect_status: 200`. See
[polling](/schema/advanced-operations/#polling).

## Auth

| Code | Cause | Fix |
| --- | --- | --- |
| `E_SESSION_REFRESH_FAILED` | An actor's login or token refresh failed | Check credentials and the auth path; also raised when the server is unreachable during login |
| `E_SECRET_ACCESS_FAILED` | The OS keychain couldn't be read | Unlock the keychain or grant access |

A *missing* keychain entry is **not** `E_SECRET_ACCESS_FAILED` — the variable is
left unresolved and you typically get a 401. Only a backend failure raises this.

## Extraction & assertions

| Code | Cause | Fix |
| --- | --- | --- |
| `E_RESPONSE_PARSE` | Response declared JSON but didn't parse | Check the real `Content-Type`; an HTML error page is the usual culprit |
| `E_EXTRACTION_FAILED` | A JSONPath / XPath / regex matched nothing | Verify the shape against a real response |
| `E_ASSERTION_FAILED` | An `assert:` predicate was false | The API changed, or the predicate is wrong |

## Polling

| Code | Cause |
| --- | --- |
| `E_POLL_TIMEOUT` | `poll_until.timeout` elapsed |
| `E_POLL_MAX_ATTEMPTS_EXCEEDED` | `max_attempts` used up |
| `E_POLL_FAIL_PREDICATE` | `fail_when` matched — the job reported failure |

None are retried: the polling loop owns its own budget, so an outer retry would
double it.

## Hooks

| Code | Cause |
| --- | --- |
| `E_HOOK_FAILURE` | A `pre_request` / `post_response` script threw |
| `E_HOOK_TIMEOUT` | A hook didn't finish in time |

## AI importer

| Code | Cause |
| --- | --- |
| `E_LLM_REQUEST_FAILED` | The model call failed |
| `E_LLM_RESPONSE_INVALID` | The model returned something unusable |

Only reachable through the [AI importer](/ai-importer/playbook/) — never during a
normal run.

## Run

| Code | Cause |
| --- | --- |
| `E_CANCELLED` | You cancelled the run |
| `E_INTERNAL` | An engine invariant broke — please [report it](https://github.com/Mirzabaig313/Reqloom/issues) |
| `E_UNKNOWN` | Unclassified |

## Which are retried

Only three: `E_NETWORK_TIMEOUT`, `E_NETWORK_DNS`, and `E_HTTP_5XX`. Everything
else fails immediately, because retrying a 401 or a failed assertion just wastes
the budget.

Retries default to 3 attempts with 500 ms backoff, doubling per attempt. Tune it
per operation:

```yaml
    retry:
      max: 5
      backoff: 1000
```

See [timeouts and retries](/schema/secrets-and-transport/#timeouts-and-retries-per-operation).

:::tip[`E_VAR_UNRESOLVED` is much faster to fix in the app]
The [desktop timeline](/desktop/running/#unresolved-variable-diagnostics) names the
unresolved variable and offers **Edit source** — jumping straight to the missing
environment variable, secret, actor, or upstream extraction — plus **Show producer
step N**. Two clicks instead of a hunt through YAML.
:::

## Blocked steps have no code

When a step fails, everything downstream is marked `BLOCK` with `err=—` — it
never ran, so it has no error of its own:

```ansi
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
  BLOCK  product.publish (0ms) err=—
  BLOCK  cart.add_item (0ms) err=—
```

Fix the first failure. In JUnit output, blocked and cancelled steps become
`<error>` while failures become `<failure>`.

## Asserting on codes in CI

Codes are stable across releases, so they're safe to grep:

```bash
reqloom run order.pay --format json --output run.json
jq -r '.steps[] | select(.error_code != null) | "\(.op) \(.error_code)"' run.json
```

```ansi
product.create E_SESSION_REFRESH_FAILED
```
