---
title: Schema specification
description: "Every key the parser reads, with its type, default, and whether it is required. The authoritative reference for reqloom.yaml."
---

Every key below is read by the parser. A key not listed here is ignored with a
warning that names the file, line and column; `reqloom lint` prints it and still
passes. A wrong **value** is an error: a wrong type, an enum value that isn't
listed (`method: DELTE`), a malformed duration, or an id defined twice fails the
load with `E_SCHEMA_INVALID`, the file and line, and the accepted values.
`method` and the inline `auth.type` match ignoring case; every other enum is
exact.

For guided introductions see the [authoring guide](/schema/authoring/); this page
is the lookup table.

## Root

```yaml
version: 1
name: MarketplaceAPI
id: b24c09e7-400e-461e-be0f-ab5f28c1c347
default_environment: local
allowed_env: [CI_API_TOKEN]
imports:
  - environments/*.yaml
  - actors/*.yaml
  - resources/*.yaml
environment: { baseUrl: http://localhost:3000 }
actors: { ... }
resources: { ... }
auth: { type: bearer, token: "{{secret.TOKEN}}" }
transport: { connect_timeout: 5s }
latency_slo: { p95_ms: 800 }
failure_message: { show: [$.error.message] }
```

| Key | Type | Required | Default |
| --- | --- | --- | --- |
| `version` | integer | **yes** | — must be 1–3 |
| `name` | string | no | `Unnamed Project` |
| `id` | string | **if the project uses secrets** | — at most 128 characters, no `/`, no control characters |
| `default_environment` | string | no | `local` |
| `allowed_env` | sequence of names | no | empty — `{{$.env.NAME}}` reads nothing |
| `imports` | sequence or map of glob patterns | no | — |
| `environment` | map | no | — becomes the default environment |
| `actors` | map of id → actor | no | — |
| `resources` | map of id → resource | no | — |
| `auth` | inline-auth map | no | — target of `auth: { type: inherit }` |
| `transport` | map | no | — applies to the **default environment only** |
| `latency_slo` | `{ p95_ms: integer }` | no | unset |
| `failure_message` | `{ show: [JSONPath, …] }` | no | — the default failure text; see [below](#failure_message) |

`id` namespaces the project's keychain secrets and is bound to the project's
folder on first use (the same folder reached through a different letter case, a
trailing separator or a symlink counts as the same folder); see [secrets](/schema/secrets-and-transport/#a-project-that-uses-secrets-needs-an-id).
An invalid `id` fails the load with `E_SCHEMA_INVALID`. `allowed_env` must be a
sequence, or the load fails.

There is **no `environments:` (plural) root key**. The singular `environment:`
defines one environment, named by `default_environment`; others are files under
`environments/`. See [file structure](/schema/file-structure/).

## Actor

```yaml
name: vendor
description: Marketplace vendor
auth: { strategy: simple, ... }
session: { ttl: 15m, refresh: { ... } }
inject: { headers: { Authorization: "Bearer {{vendor.token}}" } }
```

| Key | Type | Default |
| --- | --- | --- |
| `description` | string | `""` |
| `auth` | map | — |
| `session` | map | — |
| `inject` | map with only `headers` | — |
| `allowed_hosts` | list of hosts, each `host` or `host:port`; no scheme, path or wildcard | the host of the environment's `baseUrl` |

`inject` has no sub-key other than `headers`.

Actor, resource and environment ids must be non-empty, must not start with `.`,
and must not contain `/`, `\`, `:` or NUL; an unsafe id fails the load. An id
defined twice — in two files, or in a file and inline — fails the load naming
both places.

### `auth.strategy`

One of: `simple` (default), `chain`, `basic`, `api_key`,
`oauth2_client_credentials`, `oauth2_password`, `oauth1`, `aws_sigv4`, `bearer`,
`jwt`, `mtls`. Any other value fails the load and lists these.

Keys read per strategy:

| Strategy | Keys |
| --- | --- |
| `simple` | `method` (POST), `path`, `headers`, `body`, `expect_status` (scalar), `extract` |
| `chain` | `steps:` sequence of `{ id, method, path, headers, body, expect_status, extract }` |
| `basic` | `username`, `password` |
| `api_key` | `key`, `location` (`header`\|`query`\|`cookie`), `name` |
| `bearer` | `token` |
| `oauth2_client_credentials` | `token_url`, `client_id`, `client_secret`, `scope` |
| `oauth2_password` | the above plus `username`, `password` |
| `oauth1` | `consumer_key`, `consumer_secret`, `token`, `token_secret`, `realm` |
| `aws_sigv4` | `access_key`, `secret_key`, `region`, `service`, `session_token` |
| `jwt` | `secret`, `payload`, `algorithm` (`HS256`\|`HS512`) |
| `mtls` | `cert_path`, `format` (`pem`\|`p12`), `key_path`, `key_password`, `ca_cert_path` |

### `session`

| Key | Type | Default |
| --- | --- | --- |
| `ttl` | duration `ms`/`s`/`m`/`h`/`d`; a bare number is seconds; kept in whole seconds | `15m`; malformed input fails the load |
| `refresh` | map | — |

`refresh` reads `method` (POST), `path`, `headers`, `body`, `expect_status`
(scalar **or** sequence), `extract`. Unset `expect_status` accepts any 2xx.

For the two OAuth2 strategies, a token response with `expires_in` ends the
session at `expires_in` minus 30 seconds, or at `ttl` if that is sooner.

## Resource

```yaml
name: order
description: Customer orders
operations:
  create: { ... }
```

| Key | Type | Default |
| --- | --- | --- |
| `description` | string | `""` |
| `operations` | map of name → operation | — |

Nothing else. There is no resource-level `base_path`, `headers`, or `actor`.
Operation ids are composed as `<resource>.<operation>`.

## Operation

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `method` | string | `GET` | `GET` `POST` `PUT` `PATCH` `DELETE` `HEAD` `OPTIONS`, any case; anything else fails the load |
| `path` | string | `""` | Appended to `baseUrl`. There is no `url:` key |
| `actor` | actor id | — | Must name a defined actor, or the load fails |
| `auth` | inline-auth map | — | See below |
| `headers` | map | — | |
| `query_params` | map | — | **Not** `query:` or `params:` |
| `body` | scalar or structured | — | Scalar passes through verbatim; map/sequence → JSON. Values inside JSON strings are [escaped](/reference/variables/#json-bodies) |
| `body_form` | map | — | Form-encoded or multipart. `@path` is a file, `@@text` is text starting with `@` |
| `expect_status` | integer or sequence | — | Sequence wins over scalar |
| `poll_until` | map | — | See below |
| `for_each` | map | — | See below |
| `extract` | map | — | See below |
| `assert` | **sequence** | — | A map fails the load |
| `depends_on` | `resource.op`, or a sequence of them | — | A single id is one item; a map fails the load |
| `pre_request` | string | — | Inline JS or relative `./x.js` |
| `post_response` | string | — | Same |
| `retry` | `{ max, backoff, non_idempotent }` | 3, 500, `false` | `max` is total sends; `backoff` in **ms**, max backoff fixed at 30s. See [what is retried](/schema/secrets-and-transport/#what-is-retried) |
| `timeout` | integer **ms** | 30000 | A duration string such as `30s` fails the load |
| `force` | bool | `false` | Opt out of step caching |
| `failure_message` | `{ show: [JSONPath, …] }` | — | Replaces the project's list for this operation; see [below](#failure_message) |
| `_provenance` | map | — | Importer metadata, runtime-ignored; see [below](#_provenance) |

### `extract`

Map of variable name → path, or → `{ path, source }`.

| `source` | Auto-detected from |
| --- | --- |
| `jsonpath` (default) | anything else |
| `header` | `$.headers.` prefix |
| `cookie` | `$.cookies.` prefix |
| `status_code` | exactly `$.status_code` |
| `xpath` | never — requires the map form. **Not implemented**: never yields a value |
| `regex` | never — requires the map form |

A `[*]` in the path produces one resource **instance** per match. The JSONPath
subset and the regex limits are listed under
[advanced operations](/schema/advanced-operations/#jsonpath-in-extractions);
`$..name` is not supported. Regex patterns are checked at load.

Two operations in one resource may extract the same variable. A reference to it
then needs one clear producer, or the load fails — see
[dependency resolution](/concepts/dependencies/#when-several-operations-extract-the-same-value).

### `assert`

Sequence of bare predicates, or `{ expr, name }` maps. Items with an empty `expr`
are dropped.

Predicate grammar: `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `matches`, combined
with `&&` / `||` (or `and` / `or`, `&&` binding tighter) and grouped with
parentheses; `$.json.path` references; `$.status_code`; bare JSONPath
truthiness. Checked at load. `{{…}}` is substituted as one value that cannot
change the predicate, and a resource it names pulls its producer into the chain;
see [templates in predicates](/schema/advanced-operations/#templates-in-predicates).
Full detail in [predicate grammar](/schema/advanced-operations/#predicate-grammar).

### `poll_until`

| Key | Type | Default |
| --- | --- | --- |
| `method` | string | `GET` |
| `path` | string | `""` |
| `actor` | actor id | the parent operation's actor; must be defined |
| `success_when` | predicate | — required in practice; `{{…}}` resolved once, before the first attempt |
| `fail_when` | predicate | — wins over `success_when` |
| `interval` | duration (bare number = seconds) | `2s` |
| `backoff` | `{ base, max }` durations | max `30s`; setting `base` disables `interval` |
| `timeout` | duration (bare number = seconds) | `60s` |
| `max_attempts` | integer | `30` |

Polling engages only if the initial status matches `expect_status`, so the
sequence form is effectively required.

### `for_each`

| Key | Type | Default |
| --- | --- | --- |
| `over` | resource id | — required; a `for_each` without it, or naming an unknown resource, fails the load |
| `continue_on_error` | bool | `false` |

No alias, no `limit`, no parallelism.

### Inline `auth`

`type:` accepts `bearer`, `basic`, `apikey`/`api_key`, `aws_sigv4`/`awssigv4`/`aws`,
`oauth1`/`oauth_1`, `oauth2`/`oauth_2`/`oauth2_client_credentials`,
`jwt`/`jwt_bearer`, `mtls`/`mutual_tls`, `inherit`, and `none` for no inline
auth. The match ignores case. Any other value fails the load.

Value keys: `token`, `username`, `password`, `key`, `value`, `in`, `access_key`,
`secret_key`, `region`, `service`, `session_token`, `consumer_key`,
`consumer_secret`, `oauth_token`, `token_secret`, `grant_type`, `token_url`,
`client_id`, `client_secret`, `scope`, `client_auth`, `auth_url`, `callback_url`,
`pkce_method`, `algorithm`, `secret`, `payload`, `format`, `cert_path`,
`key_path`, `key_password`, `ca_cert_path`.

## Environment file

```yaml
name: local
variables:
  baseUrl: http://localhost:3000
  admin_password: !secret ADMIN_PASSWORD
transport:
  connect_timeout: 2s
```

Wrapped form uses `name` + `variables`. Flat form puts variables at the top level,
skipping `name` and `transport`. Without `name:`, the filename stem is used. An
empty file is skipped with a warning.

A `{{$.env.NAME}}` in an environment value, or in an actor's templates, is
checked at load: a name not in `allowed_env` fails with `E_REF_UNDEFINED`.

`!secret NAME` on a value expands to `{{secret.NAME}}`. This is the only way to
declare a secret — there is no `secrets:` block.

`baseUrl` is magic: request URLs are `baseUrl` + `path`. camelCase.

## `transport`

| Key | Type | Default |
| --- | --- | --- |
| `tls_verify` | bool | `true` |
| `tls_verify_host` | bool | `true` |
| `ca_bundle` | path | — |
| `proxy` | URL | — |
| `connect_timeout` | duration, accepts `ms`; a bare number is seconds | `5s` |
| `max_response_size` | bytes, or a whole number with `KiB`, `MiB` or `GiB` | `64MiB` |

Requests use `http` and `https` only, and redirects cap at 10. A response body
larger than `max_response_size` is aborted with `E_NETWORK_POLICY`. Any other
unit (`16MB`) or zero fails the load.

## `failure_message`

```yaml
failure_message:
  show: [$.error.message, $.error.code]
```

Chooses what a failure message quotes from a response whose status isn't in
`expect_status`. Set in `reqloom.yaml` for the project, or on an operation, where
it replaces the project's list; `show: []` on an operation means the default
text.

- Each path that matches prints as `<path>: <value>`, joined by `; `. Objects and
  arrays print as compact JSON with token-like fields masked; a path naming a
  credential field (such as `$.access_token`) prints the redaction marker.
- If nothing matches, or the body is not JSON, the default text is used: the
  response body, with secrets masked.
- The per-run size limit (`reqloom run --failure-body-size`, default 4 KiB)
  applies to the combined text.
- Paths are checked at load with the extraction JSONPath subset; `[*]` and
  `$..x` are rejected because they select many values.
- Login and token-endpoint failures always use the default text.

## `_provenance`

Importer metadata, ignored at run time. `source` is one of `hand_written`,
`openapi_import`, `postman_import`, `bruno_import`, `insomnia_import`,
`har_import`, `ai_import`, `apidog_import`, `hoppscotch_import`,
`thunder_client_import`, `httpie_import`, `http_file_import`. An unknown source
is a warning, and is written back unchanged when the project is saved.
`verified_against` is one of `none`, `openapi_example`, `postman_response`,
`insomnia_response`, `har_entry`, `synthetic`, `live_capture`; other keys are
`model`, `imported_at` and `evidence`.

## Variable grammar

Single sigil `{{ ... }}`. Scopes, in resolution order: builtins (`$.`), `env`,
`secret`, actor sessions, indexed resources (`name[N]`, 1-based), resources
(newest instance first).

Builtins: `$.uuid`, `$.now` (with signed `± duration` offset), `$.env.NAME`
(process environment, only names in `allowed_env`), `$.faker.email`,
`$.faker.phone` (any other faker name fails the load), and the codec functions
`$.base64.encode|decode`, `$.hex.encode|decode`, `$.url.encode|decode`.

Full detail in [variable syntax](/reference/variables/).

## Limits

| Limit | Value |
| --- | --- |
| Schema file size | 8 MiB per file |
| YAML body nesting depth | 64 |
| Hook script size | 1 MiB |
| Hook run time / memory | 1 s / 64 MiB |
| Upload file size | 50 MiB per file, 256 MiB per request |
| Response body | 64 MiB by default; `transport.max_response_size` |
| Redirects | 10 |
| Response JSON nesting depth | 256 |
| Predicate nesting depth | 64 |
| Regex pattern / text searched | 1024 bytes / 1 MiB |
| Project `id` length | 128 bytes |
| Env value re-expansion depth | 4 |

## Type coercion in a body

Inside a structured `body:`, quoting decides the JSON type: `"01234"` stays a
string, `01234` becomes the number `1234`; `"true"` stays a string, `true` becomes
a boolean. Quote identifiers.

## Version history

| Version | Status |
| --- | --- |
| 1 | Supported |
| 2 | Supported |
| 3 | Supported |

Anything outside 1–3 is rejected with `E_SCHEMA_VERSION`.

## See also

- [Cheat sheet](/schema/cheatsheet/) — the same information, condensed
- [Common pitfalls](/schema/pitfalls/) — what lint warns about and what it misses
- [Variable syntax](/reference/variables/) — the reference grammar in full
