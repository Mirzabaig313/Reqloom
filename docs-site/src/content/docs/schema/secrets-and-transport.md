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
headers, bodies, paths, actor auth config, poll predicates.

### Where secrets live

| Platform | Store |
| --- | --- |
| macOS | Keychain |
| Windows | Credential Manager |
| Linux | libsecret (GNOME Keyring) or KWallet |

Entries are grouped under the service **`com.reqloom.secrets`**, with the secret
name as the account. Nothing is written to your project directory, so a secret
can never end up in a commit.

### Storing a secret

**Use the desktop app.** Manage Secrets lists every `{{secret.NAME}}` your schema
references, with its keychain status, and **Set…** stores a value (entered masked,
never displayed back). See [editing schemas](/desktop/editing/#secrets).

:::caution[The CLI cannot set secrets]
`reqloom` has three subcommands — `run`, `lint`, `import` — and none of them writes
to the keychain. It only *reads* the secrets your schema references.

Without the app, use your OS keychain tool directly. On macOS:

```bash
security add-generic-password -s "com.reqloom.secrets" -a "ADMIN_PASSWORD" -w
```

Omit `-w` and it prompts, keeping the value out of your shell history.
:::

### How they're loaded

Only the secrets your schema actually references are read, and only at the start
of a real run. Reqloom scans your operations, environments, actor auth, and poll
predicates, collects the distinct `secret.X` names, and requests exactly those.
There is no bulk keychain dump.

Two consequences worth knowing:

- **Dry runs never touch the keychain.** `reqloom lint` and the desktop's chain
  preview skip secret loading entirely, so they never trigger an unlock prompt.
  Unresolved `{{secret.X}}` in a preview is expected, not a bug.
- **A missing secret is not an error.** If the keychain has no entry for the
  name, the variable is simply left unresolved and you get a request with an
  empty password — usually a 401. Only a *backend* failure (locked keychain,
  denied access, no keychain service) raises `E_SECRET_ACCESS_FAILED`.

### `{{secret.X}}` vs `{{$.env.X}}`

Two different mechanisms with confusingly similar names:

| Reference | Reads from | Redacted in logs |
| --- | --- | --- |
| `{{secret.NAME}}` | OS keychain | yes |
| `{{env.NAME}}` | your schema's `environment:` block | no |
| `{{$.env.NAME}}` | the **process** environment (`getenv`) | no |

`{{$.env.NAME}}` is handy in CI, where the runner already has secrets in the
environment:

```yaml
variables:
  api_token: "{{$.env.CI_API_TOKEN}}"
```

But it carries none of the keychain's protections. Prefer `!secret` for
developer machines and `{{$.env.X}}` only where a CI secret store is already
doing the work.

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

Durations accept `ms`, `s`, `m`, `h`, `d` — `500ms`, `10s`, `2m`. A malformed
value falls back to the default **silently**, so `connect_timeout: 10 seconds`
gets you 5s with no warning.

A `transport:` block in the root `reqloom.yaml` applies **only to the default
environment**. In a multi-environment project, put it in each environment file
instead.

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

Each step retries 3 times by default, and each attempt waits out
`connect_timeout`. Against a server that isn't running, a seven-step chain is
minutes of waiting. Shorten it while developing offline:

```yaml title="environments/local.yaml"
transport:
  connect_timeout: 100ms
```

## Timeouts and retries per operation

`connect_timeout` covers *establishing* the connection. Response time and retry
behaviour are per operation:

```yaml
  generate_report:
    method: POST
    path: /api/v1/reports
    timeout: 60000        # milliseconds
    retry:
      max: 5              # total attempts, not extra attempts
      backoff: 1000       # milliseconds, doubled per attempt
```

| Key | Type | Default |
| --- | --- | --- |
| `timeout` | integer **milliseconds** | 30000 |
| `retry.max` | integer attempts | 3 |
| `retry.backoff` | integer **milliseconds** | 500 |

:::note[These two take bare milliseconds, not duration strings]
Unlike `connect_timeout`, the operation-level `timeout` and `retry.backoff` are
plain integers. `timeout: 30s` is not valid here — write `timeout: 30000`. The
backoff doubles per attempt and is capped at 30s, which is not configurable.
:::

For an endpoint that returns `202 Accepted` and completes later, retries are the
wrong tool — use [polling](/schema/advanced-operations/#polling) instead.

## Next

- [Auth strategies](/schema/auth-strategies/) — the eleven ways to authenticate
- [Advanced operations](/schema/advanced-operations/) — polling, fan-out, assertions, hooks
- [Common pitfalls](/schema/pitfalls/) — more silent-default behaviour to watch for
