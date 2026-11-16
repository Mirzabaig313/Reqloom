---
title: Secrets, TLS & timeouts
description: "Keep credentials out of your schema with the !secret tag, and configure TLS verification, proxies, timeouts, and retries per environment."
---

Two things you'll want before pointing Reqloom at anything real: credentials
that never enter the repo, and transport settings that match your environment.

## Secrets

A schema is a file you commit. Passwords are not. The `!secret` YAML tag binds an
environment variable to your **OS keychain** instead of a literal:

```yaml title="environments/local.yaml"
name: local
variables:
  baseUrl: http://localhost:3000
  admin_email: admin@marketplace.test
  admin_password: !secret ADMIN_PASSWORD
```

`!secret ADMIN_PASSWORD` expands to the reference `{{secret.ADMIN_PASSWORD}}`.
Referencing it looks like any other variable:

```yaml title="actors/admin.yaml"
auth:
  strategy: simple
  path: /api/v1/auth/login
  body:
    email: "{{env.admin_email}}"
    password: "{{env.admin_password}}"
```

You can also write `{{secret.NAME}}` by hand anywhere a template is accepted —
headers, bodies, paths, poll paths, actor auth config, assertions and poll
conditions. In an assertion the value is substituted as a single operand, and
reports show the assertion as written, with the braces (see
[templates in predicates](/schema/advanced-operations/#templates-in-predicates)).

### A project that uses secrets needs an `id:`

Secrets are stored per project, so the project needs a stable identifier in
`reqloom.yaml`:

```yaml title="reqloom.yaml"
version: 1
name: MarketplaceAPI
id: b24c09e7-400e-461e-be0f-ab5f28c1c347
default_environment: local
```

Any value works as long as it is unique to the project, at most 128 characters,
and contains no `/` and no control characters. You rarely type it yourself:

- The **desktop app** adds a random `id:` line (right after `name:`) when it
  opens a project that has none. Nothing else in the file is touched.
- **`reqloom import`** writes one into the project it creates.
- **`reqloom lint`** prints a warning with a ready-made line to paste when the
  project references a secret and has no id.

A real run of a project that references secrets but has no `id:` fails before
any request, with `E_SCHEMA_INVALID` and a suggested id.

### Where secrets live

| Platform | Store |
| --- | --- |
| macOS | Keychain |
| Windows | Credential Manager |
| Linux | libsecret (GNOME Keyring) or KWallet |

Entries are grouped under the service **`com.reqloom.secrets`**. The account is
**`<project id>/<NAME>`** — `b24c09e7-…/ADMIN_PASSWORD` for the project above — so
two projects can each have an `ADMIN_PASSWORD` without seeing each other's.
Nothing is written to your project directory, so a secret can never end up in a
commit.

### Each project id belongs to one folder

The id comes from the project file, so a second project could copy another's id
to read its secrets. To stop that, the first time a project uses its secrets
Reqloom records which folder the id belongs to. The record lives in a per-user
file outside every project:

| Platform | File |
| --- | --- |
| macOS | `~/Library/Application Support/Reqloom/projects.json` |
| Linux | `$XDG_CONFIG_HOME/reqloom/projects.json`, else `~/.config/reqloom/projects.json` |
| Windows | `%APPDATA%\Reqloom\projects.json` |

Set `REQLOOM_CONFIG_DIR` to use `<that directory>/projects.json` instead — useful
for CI and tests.

After that, a different folder with the same id gets no secrets. The run fails
with `E_SECRET_ACCESS_FAILED`, and the message names both folders. What to do
depends on why:

- **You moved the project.** Open it in the desktop app and confirm the move in
  **Manage Secrets** ("I moved this project — use its secrets here"). The old
  folder loses access. The CLI cannot confirm a move.
- **You copied the project.** Give the copy its own `id:` (the error message
  suggests one) and enter its secrets again — they are stored under the new id.

The folder is compared as the file system sees it: a symlink to the project
folder, a path with a trailing separator, or the same folder spelled in a
different letter case (on a file system that ignores case) all count as the same
folder.

### Storing a secret

**Use the desktop app.** Manage Secrets lists every `{{secret.NAME}}` your schema
references, with its keychain status, and **Set…** stores a value (entered masked,
never displayed back). See [editing schemas](/desktop/editing/#secrets).

:::caution[The CLI cannot set secrets]
`reqloom` has three subcommands — `run`, `lint`, `import` — and none of them writes
to the keychain. It only *reads* the secrets your schema references.

Without the app, use your OS keychain tool directly, with the project id in the
account name. On macOS:

```bash
security add-generic-password -s "com.reqloom.secrets" \
  -a "b24c09e7-400e-461e-be0f-ab5f28c1c347/ADMIN_PASSWORD" -w
```

Omit the value after `-w` and it prompts, keeping it out of your shell history.
:::

### How they're loaded

Only the secrets your schema actually references are read, and only at the start
of a real run. Reqloom scans your operations, environments, and actor auth,
collects the distinct `secret.X` names, and requests exactly those. There is no
bulk keychain dump.

Two consequences worth knowing:

- **Dry runs never touch the keychain.** `reqloom lint` and the desktop's chain
  preview skip secret loading entirely, so they never trigger an unlock prompt.
  Unresolved `{{secret.X}}` in a preview is expected, not a bug.
- **A missing secret is not a keychain error.** If the keychain has no entry for
  the name, the reference stays unresolved: an operation that uses it fails with
  `E_VAR_UNRESOLVED` before sending, and a login that uses it is sent with the
  reference text and usually gets a 401. Only a *backend* failure (locked
  keychain, denied access, no keychain service) or a folder conflict raises
  `E_SECRET_ACCESS_FAILED`.
- **A build with no OS keychain backend reads no secrets.** Every
  `{{secret.X}}` is then missing, and the failure says why: `no OS keychain is
  available on this machine; {{secret.X}} cannot be read. Use a Reqloom build
  with keychain support, or pass the value through an allowed environment
  variable ({{$.env.NAME}}) instead.` Saving or deleting a secret in such a build
  fails with `E_SECRET_ACCESS_FAILED` (`secret not saved: this build has no OS
  keychain backend. …`) rather than appearing to succeed.

### Masking

Secret values are masked as `<redacted>` wherever they leave the engine: run
events, the desktop timeline, run history, error messages, and CLI output. The
one exception is a captured response body, below. Masking works two ways:

- **By value.** Every loaded `{{secret.X}}` value, every `{{$.env.NAME}}` value
  the project is allowed to read, and every credential an actor obtains is
  replaced wherever it appears — in any header, URL, or message — including its
  URL-encoded and base64 forms, so a token in a query string or a Basic
  credential is caught too.
- **By name.** Headers such as `Authorization`, `Cookie`, and `X-API-Key`, and any
  header, extracted variable, or response-body field whose name contains
  `token`, `secret`, `password`, `key`, or `auth`, are masked. This covers tokens
  in failure messages, including a failed login's response.

:::caution[Captured bodies are stored as received]
With **Capture bodies** on in the desktop app, a captured response body is shown,
and saved in run history, exactly as the API sent it — tokens included, and that
includes a login's response. Headers, URLs, failure messages and extracted
values stay masked. The run history file then holds those tokens in clear text.
The desktop app turns Capture bodies **on** when it starts; untick it before
running against an API whose responses carry real credentials. The CLI never
captures bodies.
:::

### Failure messages

When a response's status isn't in `expect_status`, the failure message quotes the
API's response, with secrets masked by value and by name:

```ansi
HTTP 422 (expected 201) — {"error":{"message":"email is taken","code":"conflict"}}
```

A run quotes at most 4 KiB of response text per failure; `reqloom run
--failure-body-size` changes it (up to 1 MiB, `0` for none). A longer response is
cut and marked, for example `… (truncated, 2.3 MiB total)`. A body that is not
text, such as binary data or invalid UTF-8, is not quoted.

To quote only the useful parts, list JSONPaths under `failure_message.show`, in
`reqloom.yaml` for the whole project or on an operation, where it replaces the
project's list:

```yaml title="reqloom.yaml"
failure_message:
  show: [$.error.message, $.error.code]
```

```ansi
HTTP 422 (expected 201) — $.error.message: email is taken; $.error.code: conflict
```

Each matching path prints as `<path>: <value>`, joined by `; `. An object or
array prints as compact JSON with token-like fields masked, and a path that names
a credential field prints `<redacted>`. When nothing matches, or the body isn't
JSON, the default text is used, and `show: []` on an operation asks for the
default text too. The size limit applies to the combined text. Paths are checked
when the project loads; `[*]` and `$..x` are rejected because they select many
values. A failed login or token request always uses the default text.

Known limits:

- Values shorter than 6 characters are not masked by value.
- A token extracted under an ordinary name from an ordinary field — say
  `sid: $.data.sid` — is not recognised as a secret, so it is not masked.
  Extract it under a name containing `token` (or one of the words above) if it
  matters.

### `{{secret.X}}` vs `{{$.env.X}}`

Two different mechanisms with confusingly similar names:

| Reference | Reads from | Masked in output |
| --- | --- | --- |
| `{{secret.NAME}}` | OS keychain | yes |
| `{{env.NAME}}` | your schema's `environment:` block | only when it holds a `!secret` |
| `{{$.env.NAME}}` | the **process** environment, for names in `allowed_env` | yes |

`{{$.env.NAME}}` is handy in CI, where the runner already has secrets in the
environment. A project may read only the variables it lists under
`allowed_env:` in `reqloom.yaml`:

```yaml title="reqloom.yaml"
allowed_env:
  - CI_API_TOKEN
```

```yaml title="environments/ci.yaml"
variables:
  api_token: "{{$.env.CI_API_TOKEN}}"
```

A name that isn't listed does not resolve. `reqloom lint` and project load
reject it:

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'o.a' reads $.env.HOME, but environment
variable HOME is not in allowed_env in reqloom.yaml; add it to the allowed_env
list there
```

`allowed_env:` lives in the project file, so it guards against accidents — a
schema quietly reading `AWS_SECRET_ACCESS_KEY` — not against a project that was
edited to add a name. Prefer `!secret` on developer machines and `{{$.env.X}}`
only where a CI secret store is already doing the work.

## Transport

The `transport:` block configures TLS, proxying, and connection timeouts. Put it
in an environment file so each environment can differ:

```yaml title="environments/staging.yaml"
name: staging
variables:
  baseUrl: https://staging.internal.example.com
transport:
  ca_bundle: ./certs/internal-ca.pem
  connect_timeout: 10s
```

| Key | Type | Default | Purpose |
| --- | --- | --- | --- |
| `tls_verify` | bool | `true` | Verify the server certificate chain |
| `tls_verify_host` | bool | `true` | Verify the hostname matches the certificate |
| `ca_bundle` | path | — | Trust a private CA |
| `proxy` | URL | — | Route requests through a proxy |
| `connect_timeout` | duration | `5s` | Time allowed to establish a connection |
| `max_response_size` | bytes, or `KiB` / `MiB` / `GiB` | `64MiB` | Largest response body accepted |

Durations accept `ms`, `s`, `m`, `h`, `d` — `500ms`, `10s`, `2m`. A bare number
is whole seconds: `connect_timeout: 30` is 30 seconds. A malformed value, such as
`connect_timeout: 10 seconds`, fails the load with the file and line.
`max_response_size` takes a whole number of bytes or a whole number with `KiB`,
`MiB` or `GiB` (`16MiB`); `16MB` or `0` fails the load.

A `transport:` block in the root `reqloom.yaml` applies **only to the default
environment**. In a multi-environment project, put it in each environment file
instead.

### Limits

| Limit | Value | When exceeded |
| --- | --- | --- |
| URL schemes | `http` and `https` only, on every redirect hop too | `E_NETWORK_POLICY` |
| Response body | 64 MiB by default, counted after decompression; set with `max_response_size` | `E_NETWORK_POLICY` |
| Redirects | 10, not configurable | `E_NETWORK_POLICY` |

`E_NETWORK_POLICY` is never retried.

### Self-signed certificates

For a local server with a self-signed certificate:

```yaml title="environments/local.yaml"
transport:
  tls_verify: false
  tls_verify_host: false
```

:::danger[Never disable verification against a shared environment]
`tls_verify: false` accepts any certificate, which defeats TLS entirely and
makes the connection trivially interceptable. Use it against `localhost` only.
For internal environments with a private CA, point `ca_bundle` at the CA
certificate instead — you keep verification and stop the errors.
:::

### Faster failures while offline

A step can be sent up to 3 times by default, and an attempt against a host that
doesn't answer waits out `connect_timeout`. Against a server that isn't running,
a long chain spends most of its time waiting. Shorten it while developing
offline:

```yaml title="environments/local.yaml"
transport:
  connect_timeout: 100ms
```

## Timeouts and retries per operation

`connect_timeout` covers *establishing* the connection. Response time and retry
behaviour are per operation:

```yaml
  generate_report:
    method: GET
    path: /api/v1/reports/latest
    timeout: 60000        # milliseconds
    retry:
      max: 5              # total sends, including the first
      backoff: 1000       # milliseconds, doubled per attempt
```

| Key | Type | Default |
| --- | --- | --- |
| `timeout` | integer **milliseconds** | 30000 |
| `retry.max` | integer — total number of sends | 3 |
| `retry.backoff` | integer **milliseconds** | 500 |
| `retry.non_idempotent` | `true` or `false` | `false` |

`retry.max` counts every send: `max: 3` sends at most three times, and `max: 1`
turns retries off. Values below 1 count as 1.

:::note[These two take bare milliseconds, not duration strings]
Unlike `connect_timeout`, the operation-level `timeout` and `retry.backoff` are
plain integers in milliseconds. `timeout: 30s` fails the load — write
`timeout: 30000`. The backoff doubles per attempt and is capped at 30s, which is
not configurable.
:::

### What is retried

Within the `retry.max` budget, a failed attempt is sent again when:

| Failure | GET, HEAD, OPTIONS, PUT, DELETE | POST, PATCH |
| --- | --- | --- |
| `E_NETWORK_DNS` or `E_NETWORK_CONNECT` — the request never reached the server | retried | retried |
| `E_NETWORK_TIMEOUT` — any other transport failure | retried | only with `non_idempotent: true` |
| A 5xx response not listed in `expect_status` | retried | only with `non_idempotent: true` |

Everything else is final: 4xx responses, `E_NETWORK_TLS`, `E_NETWORK_POLICY`, a
5xx you listed in `expect_status`, assertion and extraction failures. An
operation with no `expect_status` accepts every status, so a 5xx is never retried
there — set `expect_status` to get 5xx retries.

POST and PATCH are not safe to repeat — a second send can create a second order
or charge a card twice — so they are re-sent only when the request provably never
left your machine. A DNS or connect failure that happens after a redirect hop
counts as `E_NETWORK_TIMEOUT`, because the first hop may already have delivered
the request.

If an endpoint is safe to repeat — it takes an idempotency key, say — opt in:

```yaml
  charge:
    method: POST
    path: /api/v1/charges
    headers:
      Idempotency-Key: "{{$.uuid}}"
    retry:
      max: 3
      non_idempotent: true
```

`non_idempotent` must be written as `true` or `false`; anything else fails the
load with `E_SCHEMA_INVALID`. It has no effect on other methods.

For an endpoint that returns `202 Accepted` and completes later, retries are the
wrong tool — use [polling](/schema/advanced-operations/#polling) instead.

## Next

- [Auth strategies](/schema/auth-strategies/) — the eleven ways to authenticate
- [Advanced operations](/schema/advanced-operations/) — polling, fan-out, assertions, hooks
- [Common pitfalls](/schema/pitfalls/) — what lint warns about and what it misses
