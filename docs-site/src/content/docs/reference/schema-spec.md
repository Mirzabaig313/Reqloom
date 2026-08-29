---
title: Schema specification
description: "Every key the parser reads, with its type, default, and whether it is required. The authoritative reference for reqloom.yaml."
---

Every key below is read by the parser. Anything not listed here is **ignored
silently** — there is no strict mode, so a misspelled key is dropped rather than
reported.

For guided introductions see the [authoring guide](/schema/authoring/); this page
is the lookup table.

## Root

```yaml
version: 1
name: MarketplaceAPI
default_environment: local
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
```

| Key | Type | Required | Default |
| --- | --- | --- | --- |
| `version` | integer | **yes** | — must be 1–3 |
| `name` | string | no | `Unnamed Project` |
| `default_environment` | string | no | `local` |
| `imports` | sequence or map of glob patterns | no | — |
| `environment` | map | no | — becomes the default environment |
| `actors` | map of id → actor | no | — |
| `resources` | map of id → resource | no | — |
| `auth` | inline-auth map | no | — target of `auth: { type: inherit }` |
| `transport` | map | no | — applies to the **default environment only** |
| `latency_slo` | `{ p95_ms: integer }` | no | unset |

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

`inject` has no sub-key other than `headers`.

### `auth.strategy`

One of: `simple` (default), `chain`, `basic`, `api_key`,
`oauth2_client_credentials`, `oauth2_password`, `oauth1`, `aws_sigv4`, `bearer`,
`jwt`, `mtls`. An unrecognised value **falls back to `simple`**.

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
| `ttl` | duration `s`/`m`/`h`/`d` | `15m` — malformed input also yields 15m |
| `refresh` | map | — |

`refresh` reads `method` (POST), `path`, `headers`, `body`, `expect_status`
(scalar **or** sequence), `extract`. Unset `expect_status` accepts any 2xx.

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
| `method` | string | `GET` | Unknown value → `GET` |
| `path` | string | `""` | Appended to `baseUrl`. There is no `url:` key |
| `actor` | actor id | — | |
| `auth` | inline-auth map | — | See below |
| `headers` | map | — | |
| `query_params` | map | — | **Not** `query:` or `params:` |
| `body` | scalar or structured | — | Scalar passes through verbatim; map/sequence → JSON |
| `body_form` | map | — | Form-encoded or multipart |
| `expect_status` | integer or sequence | — | Sequence wins over scalar |
| `poll_until` | map | — | See below |
| `for_each` | map | — | See below |
| `extract` | map | — | See below |
| `assert` | **sequence** | — | A map is ignored |
| `depends_on` | **sequence** of `resource.op` | — | A scalar is ignored |
| `pre_request` | string | — | Inline JS or relative `./x.js` |
| `post_response` | string | — | Same |
| `retry` | `{ max, backoff }` | 3, 500 | `backoff` in **ms**; max backoff fixed at 30s |
| `timeout` | integer **ms** | 30000 | Not a duration string |
| `force` | bool | `false` | Opt out of step caching |
| `_provenance` | map | — | Importer metadata, runtime-ignored |

### `extract`

Map of variable name → path, or → `{ path, source }`.

| `source` | Auto-detected from |
| --- | --- |
| `jsonpath` (default) | anything else |
| `header` | `$.headers.` prefix |
| `cookie` | `$.cookies.` prefix |
| `status_code` | exactly `$.status_code` |
| `xpath` | never — requires the map form |
| `regex` | never — requires the map form |

A `[*]` in the path produces one resource **instance** per match.

### `assert`

Sequence of bare predicates, or `{ expr, name }` maps. Items with an empty `expr`
are dropped.

Predicate grammar: `==`, `!=`, `<`, `<=`, `>`, `>=`, `in`, `matches`, combined
with `&&` / `||`; `$.json.path` references; `$.status_code`; bare JSONPath
truthiness.

### `poll_until`

| Key | Type | Default |
| --- | --- | --- |
| `method` | string | `GET` |
| `path` | string | `""` |
| `actor` | actor id | the parent operation's actor |
| `success_when` | predicate | — required in practice |
| `fail_when` | predicate | — wins over `success_when` |
| `interval` | duration | `2s` |
| `backoff` | `{ base, max }` | max `30s`; setting `base` disables `interval` |
| `timeout` | duration | `60s` |
| `max_attempts` | integer | `30` |

Polling engages only if the initial status matches `expect_status`, so the
sequence form is effectively required.

### `for_each`

| Key | Type | Default |
| --- | --- | --- |
| `over` | resource id | — required; block dropped without it |
| `continue_on_error` | bool | `false` |

No alias, no `limit`, no parallelism.

### Inline `auth`

`type:` accepts `bearer`, `basic`, `apikey`/`api_key`, `aws_sigv4`/`awssigv4`/`aws`,
`oauth1`/`oauth_1`, `oauth2`/`oauth_2`/`oauth2_client_credentials`,
`jwt`/`jwt_bearer`, `mtls`/`mutual_tls`, `inherit`. Anything else, including
`none`, means no inline auth.

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
skipping `name` and `transport`. Without `name:`, the filename stem is used.

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
| `connect_timeout` | duration, accepts `ms` | `5s` |

## Variable grammar

Single sigil `{{ ... }}`. Scopes, in resolution order: builtins (`$.`), `env`,
`secret`, actor sessions, indexed resources (`name[N]`, 1-based), resources
(newest instance first).

Builtins: `$.uuid`, `$.now` (with signed `± duration` offset), `$.env.NAME`
(process environment), `$.faker.email`, `$.faker.phone`, and the codec functions
`$.base64.encode|decode`, `$.hex.encode|decode`, `$.url.encode|decode`.

Full detail in [variable syntax](/reference/variables/).

## Limits

| Limit | Value |
| --- | --- |
| Schema file size | 8 MiB per file |
| YAML body nesting depth | 64 |
| Hook script size | 1 MiB |
| Upload file size | 50 MiB |
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
- [Common pitfalls](/schema/pitfalls/) — where silent defaults bite
- [Variable syntax](/reference/variables/) — the reference grammar in full
