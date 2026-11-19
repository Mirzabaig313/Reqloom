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
| `E_SCHEMA_INVALID` | Structurally valid YAML the engine can't accept — a wrong type (`timeout: 30s`), an unknown enum value (`method: DELTE`, an unknown `auth.strategy`), a malformed duration or size, `assert` as a map, `for_each` without `over`, an actor, resource or environment defined twice or with an unsafe id, a bad hook path, a malformed assertion or poll condition, a refused regex, an unsupported `failure_message.show` path, an invalid `id:`, or a secret-using project with no `id:` (at run time) | Read the detail; it starts with `file:line:col` and, for an enum, lists the accepted values |
| `E_SCHEMA_VERSION` | `version:` is missing or outside 1–3 | Set a supported `version:` |
| `E_REF_UNDEFINED` | A `{{X.y}}` scope (in a request, an assertion or a poll condition), a `depends_on` target, an operation's or poll's `actor`, or a `for_each` resource doesn't exist; an unknown `$.faker` name; a `$.env` name not in `allowed_env`, in an operation, an environment value or an actor; or an ambiguous producer | Fix the name — the message names the symbol and the file and line |
| `E_CYCLE` | Circular dependency | Break the loop; the message prints the path |

```ansi
LINT FAIL [E_SCHEMA_INVALID]: ./reqloom.yaml:11:17: unknown method 'PSOT'; accepted values: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS
LINT FAIL [E_CYCLE]: Circular dependency detected: order.one → order.two
```

Unknown keys are not errors: they are warnings that `reqloom lint` prints and
passes. See [lint warnings](/cli/lint/#warnings).

## Resolution

Raised while building a request.

| Code | Cause | Fix |
| --- | --- | --- |
| `E_VAR_UNRESOLVED` | A reference in a request, assertion or poll condition couldn't be substituted | Most often a typo'd extraction name, a missing env var, or a producer that returned `null` — the detail's `Cause:` line says which |
| `E_UPLOAD_FILE_UNREADABLE` | A `body_form` `@path` is empty, missing, not a regular file, over 50 MiB, or pushes the request's files past 256 MiB | A relative path resolves against the project folder (the one holding `reqloom.yaml`), not where you run the command |
| `E_INDEXED_REF_OUT_OF_RANGE` | **Reserved — never raised.** An out-of-range `{{res[N].x}}` surfaces as `E_VAR_UNRESOLVED` | — |

`E_VAR_UNRESOLVED` is the one you'll meet most. Remember that
[lint validates the scope, not the field](/cli/lint/#what-it-does-not-check) —
`{{order.typo}}` passes lint whenever a resource named `order` exists, then
fails here. The detail names the variable, where it was used, and the cause:

```ansi
Variable: {{user.user_id}}
Location: URL path
Cause: The last user.nulls response returned null for 'user_id'.
```

## Network

| Code | Cause | Retried |
| --- | --- | --- |
| `E_NETWORK_DNS` | Hostname didn't resolve; the request never left your machine | yes, any method |
| `E_NETWORK_CONNECT` | The connection (or proxy tunnel) couldn't be established; no request bytes reached the server | yes, any method |
| `E_NETWORK_TIMEOUT` | A connect or read timeout, or any other transport failure after which the request may have reached the server | yes, except POST/PATCH without `retry.non_idempotent` |
| `E_NETWORK_TLS` | TLS handshake or certificate verification failed | no |
| `E_NETWORK_POLICY` | Refused by a transport rule: a scheme other than `http`/`https`, a response body over the limit (64 MiB unless `transport.max_response_size` says otherwise), or more than 10 redirects. The detail says which | no |

A DNS or connect failure after a redirect hop is reported as `E_NETWORK_TIMEOUT`,
because an earlier hop may already have delivered the request.

`E_NETWORK_TLS` against an internal host usually means a private CA — point
`transport.ca_bundle` at it rather than disabling verification. See
[TLS settings](/schema/secrets-and-transport/#transport).

## HTTP

| Code | Cause | Retried |
| --- | --- | --- |
| `E_HTTP_5XX` | A 5xx status not listed in `expect_status` | yes, except POST/PATCH without `retry.non_idempotent` |
| `E_HTTP_4XX` | Any other status not listed in `expect_status` — a 4xx, but also a 2xx or 3xx you didn't expect | no |
| `E_STATUS_MISMATCH` | **Reserved — never raised.** A status mismatch surfaces as `E_HTTP_4XX` or `E_HTTP_5XX` | — |

Both HTTP codes need an `expect_status` to compare against. Without one, every
status counts as success — a `500` included — and nothing is retried. The detail
quotes the response, with secrets masked and up to 4 KiB by default; see
[failure messages](/schema/secrets-and-transport/#failure-messages).

`E_HTTP_4XX` on a `202` is the classic polling mistake: an async endpoint needs
`expect_status: [202, 200]`, not `expect_status: 200`. See
[polling](/schema/advanced-operations/#polling).

## Auth

| Code | Cause | Fix |
| --- | --- | --- |
| `E_SESSION_REFRESH_FAILED` | An actor's login or token refresh failed, or a request couldn't be signed (such as a SigV4 multipart upload to a service other than S3) | For a login, the detail starts `actor '<id>' could not sign in:` and gives the cause; also raised when the server is unreachable during login |
| `E_SECRET_ACCESS_FAILED` | The OS keychain couldn't be read, the project's `id:` belongs to a different folder on this machine, or a secret couldn't be saved or removed (including in a build with no keychain backend) | Unlock the keychain or grant access; for a folder conflict, see [each project id belongs to one folder](/schema/secrets-and-transport/#each-project-id-belongs-to-one-folder) |
| `E_HOST_NOT_APPROVED` | A request would have carried a credential, secret, session cookie or local file to a host (`scheme://host:port`) you have not approved for this project, over TLS that is not verified or trusts a project-named CA without your approval, or to a host outside the `allowed_hosts` of an actor whose values it uses (or, without it, other than the `baseUrl` host). Raised before the request or redirect hop is sent; nothing was sent | Approve the host when asked, with `reqloom hosts approve <host>` at a terminal, in the desktop app, or for CI with `REQLOOM_APPROVED_HOSTS`; for an `allowed_hosts` refusal, list the host if it is meant to receive credentials. See [hosts that may receive credentials](/schema/secrets-and-transport/#hosts-that-may-receive-credentials) |

A *missing* secret is **not** `E_SECRET_ACCESS_FAILED`: when neither
`REQLOOM_SECRET_<NAME>` nor the keychain has it, the step that needs it — a login
included — fails with `E_VAR_UNRESOLVED` before sending, and the detail names the
variable to set. Only a backend failure or a folder conflict raises this on a run.
With no keychain to read (a build without one, or a machine without a keychain
service) the `E_VAR_UNRESOLVED` detail starts `no secret backend available` and,
in the CLI, names the `REQLOOM_SECRET_<NAME>` variable to set (the name in upper
case). See
[secrets on CI](/schema/secrets-and-transport/#on-ci-and-in-agent-sandboxes-environment-variables).

## Extraction & assertions

| Code | Cause | Fix |
| --- | --- | --- |
| `E_RESPONSE_PARSE` | Response declared JSON but didn't parse, or nests deeper than 256 levels | Check the real `Content-Type`; an HTML error page is the usual culprit |
| `E_EXTRACTION_FAILED` | A JSONPath or regex matched nothing, or the JSONPath uses syntax the engine doesn't support (such as `$..name`) | Verify the shape against a real response; see [supported JSONPath](/schema/advanced-operations/#jsonpath-in-extractions) |
| `E_ASSERTION_FAILED` | An `assert:` predicate was false; the output [says what was compared](/cli/run/#a-failed-assertion-says-why) | The API changed, or the predicate is wrong. An unresolved `{{…}}` in an assertion is `E_VAR_UNRESOLVED` instead |

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
| `E_HOOK_FAILURE` | A `pre_request` / `post_response` script threw, returned a rejected promise, or ran out of its 64 MiB memory |
| `E_HOOK_TIMEOUT` | A hook didn't finish within 1 second |

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
| `E_CANCELLED` | You cancelled the run. A request in flight is stopped within about a second and is not retried |
| `E_INTERNAL` | An engine invariant broke — please [report it](https://github.com/Mirzabaig313/Reqloom/issues) |
| `E_UNKNOWN` | Fallback string for a code this build doesn't know; not expected in normal output |

## Import

| Code | Cause |
| --- | --- |
| `E_PROJECT_EXISTS` | The import target already has a `reqloom.yaml` and overwriting wasn't asked for |
| `E_WRITE_FAILED` | The imported project couldn't be written to disk |

## Command line

Raised by the `reqloom` command itself, before the engine runs. They appear as
`error.code` in [JSON output](/cli/run/#json) and in the JUnit `<error>`.

| Code | Cause | Exit code |
| --- | --- | --- |
| `E_CLI_USAGE` | Unknown option, or a missing or invalid value | `2` |
| `E_CLI_PROJECT_NOT_FOUND` | No `reqloom.yaml` at the project path | `2` |
| `E_CLI_UNKNOWN_OPERATION` | The operation isn't in the project; the message lists the ones that are | `2` |
| `E_CLI_UNKNOWN_ENVIRONMENT` | `--env` names an environment the project doesn't define | `2` |
| `E_CLI_INTERNAL` | An unexpected error in the command | `3` |
| `E_CLI_NO_HISTORY` | `show`: the project has no run history yet, or it cannot be read | `2` (`3` when unreadable) |
| `E_CLI_UNKNOWN_RUN` | `show`: no run with that id, or a run the desktop app started (not printable from the CLI) | `2` |
| `E_CLI_UNKNOWN_STEP` | `show`: the run has no such step; the message lists its steps | `2` |
| `E_CLI_PATH_NOT_FOUND` | `show --path`: the path selects nothing, or the recorded body was cut or is not JSON | `1` |

## Which are retried

Four codes can be retried: `E_NETWORK_DNS`, `E_NETWORK_CONNECT`,
`E_NETWORK_TIMEOUT`, and `E_HTTP_5XX` (a 5xx you didn't list in
`expect_status`). Everything else fails immediately, because retrying a 401 or a
failed assertion just wastes the budget.

The method matters. GET, HEAD, OPTIONS, PUT, and DELETE are retried on all four.
POST and PATCH are retried only on `E_NETWORK_DNS` and `E_NETWORK_CONNECT` —
failures where the request never reached the server — unless the operation sets
`retry.non_idempotent: true`.

`retry.max` is the total number of sends, 3 by default, with a 500 ms backoff
doubling per attempt. Tune it per operation:

```yaml
    retry:
      max: 5
      backoff: 1000
```

See [what is retried](/schema/secrets-and-transport/#what-is-retried).

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
