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

No environment variable moves this file: it also holds the hosts you approved
and the setting that stops the command line approving hosts, so a program that
can set variables for the commands it runs must not be able to point Reqloom at
a fresh, empty copy. The home directory comes from your user account, not from
`$HOME` (Windows still uses `%APPDATA%`). A file Reqloom cannot read as a JSON
object is treated as damaged: nothing in it counts, the command line cannot
approve hosts, and Reqloom will not overwrite it until you repair it or move it
aside.

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

### On CI and in agent sandboxes: environment variables

A CI runner or a cloud sandbox for an AI agent usually has no OS keychain. There,
supply each secret to the `reqloom` CLI as an environment variable named
`REQLOOM_SECRET_` followed by the secret's name in upper case:

| Reference | Environment variable |
| --- | --- |
| `{{secret.ADMIN_PASSWORD}}` | `REQLOOM_SECRET_ADMIN_PASSWORD` |
| `{{secret.stripe_key}}` | `REQLOOM_SECRET_STRIPE_KEY` |

```yaml title=".github/workflows/api-tests.yml"
- name: Run API workflow tests
  env:
    REQLOOM_SECRET_ADMIN_PASSWORD: ${{ secrets.ADMIN_PASSWORD }}
    REQLOOM_SECRET_VENDOR_PASSWORD: ${{ secrets.VENDOR_PASSWORD }}
  run: reqloom run refund.approve --project api-tests --format junit --output results.xml
```

In a CLI run, a `{{secret.NAME}}` is looked up in this order, and the first hit
wins:

1. The environment variable `REQLOOM_SECRET_<NAME>`.
2. The OS keychain.
3. Neither: the step that needs it fails with `E_VAR_UNRESOLVED`, naming the
   secret, before anything is sent.

The details:

- **The desktop app never reads these variables.** It uses the keychain only, even
  when it was started from a terminal that has `REQLOOM_SECRET_*` set, so the
  environment can never override a value saved in Manage Secrets.
- **Only the prefixed name is read.** A plain `ADMIN_PASSWORD` variable is never
  consulted, so a project cannot pick up `PATH`, `HOME` or another variable that
  happens to share a secret's name.
- **The name is upper-cased.** `api_key` is read from `REQLOOM_SECRET_API_KEY`;
  `REQLOOM_SECRET_api_key` is never read. Only names made of ASCII letters,
  digits and `_` map to a variable. A secret such as `api-key` can come only from
  the keychain; `reqloom lint` warns about it and suggests a name (`api_key`).
- **Two names cannot share a variable.** `api_key` and `API_KEY` both map to
  `REQLOOM_SECRET_API_KEY`. `reqloom lint` warns about the pair, and a CLI run
  with that variable set fails before sending anything, with `E_SCHEMA_INVALID`
  naming both secrets, rather than give both the same value.
- **An empty variable counts as unset.** CI systems often turn a missing secret
  into an empty string; the failure then says `REQLOOM_SECRET_<NAME> is set but
  empty` instead of sending an empty credential.
- **A value from the environment never touches the keychain.** That secret causes
  no keychain read and no unlock prompt. When every secret the project uses comes
  from the environment, the keychain is not opened at all and the project's `id:`
  and folder are not checked, so a project without an `id:` can use secrets this
  way.
- **Masked like any secret.** The value is replaced by `<redacted>` in every
  output, exactly as a keychain value is.

### How they're loaded

Only the secrets your schema actually references are read, and only at the start
of a real run. Reqloom scans your operations, environments, and actor auth,
collects the distinct `secret.X` names, takes those it finds in
`REQLOOM_SECRET_<NAME>` variables, and requests exactly the rest from the
keychain. There is no bulk keychain dump.

Two consequences worth knowing:

- **Dry runs never touch the keychain.** `reqloom lint` and the desktop's chain
  preview skip secret loading entirely, so they never trigger an unlock prompt.
  Unresolved `{{secret.X}}` in a preview is expected, not a bug.
- **A missing secret is not a keychain error.** If neither the environment nor
  the keychain has the name, the reference stays unresolved: the step that uses
  it fails with `E_VAR_UNRESOLVED` before sending — a login included, so it never
  reaches the API as a confusing 401. The message names the variable to set:
  `the secret 'ADMIN_PASSWORD' has no value. Set REQLOOM_SECRET_ADMIN_PASSWORD,
  or store it with Manage Secrets in the desktop app.` (the desktop app's message
  points at Manage Secrets only). Only a *backend* failure (locked keychain,
  denied access) or a folder conflict raises `E_SECRET_ACCESS_FAILED`; in the CLI
  its message also names the variable that would supply the secret without the
  keychain.
- **No secret backend is never silent.** When there is no keychain to read — a
  build without keychain support, or a machine where the keychain service isn't
  running, such as a headless Linux box — the first missing secret says so. In
  the CLI: `no secret backend available; set REQLOOM_SECRET_ADMIN_PASSWORD or run
  on a desktop session with a keychain.` In the desktop app, which reads no
  environment: `no secret backend available; {{secret.ADMIN_PASSWORD}} cannot be
  read here. Run on a desktop session with a keychain, or run the reqloom CLI with
  REQLOOM_SECRET_ADMIN_PASSWORD set.` A keychain that could not be reached is
  tried again on the next run. Saving or deleting a secret in a build with no
  keychain backend fails with `E_SECRET_ACCESS_FAILED` (`secret not saved: this
  build has no OS keychain backend. …`) rather than appearing to succeed.

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
- **By use, in CLI output only.** The `reqloom` CLI can be read by an AI agent,
  so it masks more than the desktop app. The desktop app and run history show
  exactly what they always did; everything below applies only to CLI output.
  - *Field names.* Besides the names above, any response field or extracted
    variable whose name contains `authorization`, `cookie`, `token`, `secret`,
    `password`, `passwd`, `apikey`, `accesskey`, `privatekey`, `credential`,
    `session`, `bearer` or `jwt`, or is exactly `auth` or `key` — so `jwt` and
    `session_id` are masked even when the run never sends them.
  - *Extractions the project sends as credentials.* An extraction is masked from
    the moment it is extracted when any template in the project places it in a
    credential position: a header the display policy masks or that has a
    credential name (`Authorization`, `X-Auth-Key`,
    `Ocp-Apim-Subscription-Key`, `Authentication`, …), a query parameter with a
    credential name (including `key` and `auth`), a request-body field with a
    credential name (but not one named exactly `key` or `auth`, or ending in
    `type` or `hint`, such as `{"Key": "Environment"}` or `token_type_hint`), inline
    auth, or an actor's login, refresh or injected headers.
  - *Values actually sent* in those positions (the token after `Bearer`, the user
    and password inside `Basic`), as a backstop for values a hook sets.
  - *Cookies.* Every cookie value a request sends or a response sets with
    `Set-Cookie`, whatever its name: no name reliably tells a session cookie
    from a preference, and missing a session cookie would leak it.
  - *Escaped secrets.* A secret containing `"` or `\` is still found inside a JSON
    response, which carries it escaped.
  - *Keys stay.* Only values are replaced; a JSON key is never rewritten.

  So a session id extracted as `sid` from `$.data.sid` and sent as
  `Authorization: Bearer {{session.sid}}` never appears in `reqloom run` output,
  neither in the response it came from nor in its `extracted` entry, while
  `keyword`, `author` or a `Key`/`Value` tag keep their values readable.

Values shorter than 6 characters are not masked by value.

:::caution[Captured bodies are stored as received]
With **Capture bodies** on in the desktop app, a captured response body is shown,
and saved in run history, exactly as the API sent it — tokens included, and that
includes a login's response. Headers, URLs, failure messages and extracted
values stay masked. The run history file then holds those tokens in clear text.
The desktop app turns Capture bodies **on** when it starts; untick it before
running against an API whose responses carry real credentials. The CLI shows
response bodies only in `reqloom run --format json`, and there they are always
masked with the CLI rules above; no flag turns that off.
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

## Hosts that may receive credentials

A project names its hosts in files anyone can edit, an AI agent included. So
Reqloom sends an actor's credentials, a secret or a session cookie to a host
only after **you** have approved that host for that project. The project file
can never approve a host by itself.

### What needs approval

Whether a request carries a credential is decided first from what the operation
**refers to**, then from the bytes about to be sent, so a transformed value
(`{{$.hex.encode(secret.KEY)}}`) or a hook's rewrite still counts. A request
needs an approved host when:

- it is sent as an actor (an operation run as the actor, its sign-in, refresh,
  or OAuth2 token request), or its templates use any actor's values
  (`{{admin.access_token}}`), even through a `$.` function;
- its templates or its pre-request hook use a `{{secret.X}}` (from the keychain
  or from `REQLOOM_SECRET_<NAME>`), a `{{$.env.X}}`, an environment value that is
  a secret, inline or default auth, or a value the project sends as a credential
  elsewhere;
- it uploads a local file (`body_form` `@path`): any file, since it can hold
  anything; the question names the file;
- it carries a `Cookie` header, or a header, query parameter or body field whose
  name looks like a credential (`Authorization`, `X-Api-Key`, `key`, `password`,
  `token`, …), or the value of a secret or session token anywhere;
- it presents a client certificate.

A request that carries none of these, such as a public `GET`, needs no approval,
so in a chain such steps before the first refused one are still sent. Secret
values shorter than 4 characters cannot be recognised by value.

### How hosts are compared

A host is approved as `scheme://host:port`: `https://API.example.com/v1` is
`https://api.example.com:443`, `http://127.0.0.1:3000` is
`http://127.0.0.1:3000`. Approving `https://` never covers `http://` on the same
host, even on the same port. The name is compared without case and without a
trailing dot. An IP address is compared as the URL parser normalises it
(`127.1` is `127.0.0.1`; an IPv6 zone is ignored). A name with non-ASCII
characters cannot be approved: write it in its `xn--` form. There are no
wildcards. `localhost` and `127.0.0.1` are different hosts, and neither is
approved automatically.

For an `https://` host, a request that turns TLS verification off
(`tls_verify: false`) or trusts a certificate authority the project names
(`ca_bundle`) is a separate question even when the host is approved, since
someone in between could read the credential. Without a terminal it is
refused.

### Restricting an actor: `allowed_hosts`

```yaml title="actors/vendor.yaml"
vendor:
  auth:
    strategy: oauth2_client_credentials
    token_url: https://auth.example.com/oauth/token
    client_id: shop
    client_secret: "{{secret.VENDOR_CLIENT_SECRET}}"
  allowed_hosts:
    - api.example.com          # any port
    - auth.example.com:443     # this port only
```

An actor's credentials, and any request that uses its values, go only to the
hosts in its `allowed_hosts` (host and optional port, no scheme). Without the
list, they go only to the host of the environment's `baseUrl`. The list can only
restrict: each host on it still needs your approval, and a host not on it is
refused without asking. An actor whose `token_url` or sign-in is on a different
host than `baseUrl` must therefore list both.

### Where approvals are kept

In your user settings, next to the record of which folder owns each project id
(`projects.json` in Reqloom's config folder, which no environment variable
moves), keyed by the project's `id:`. A project needs an `id:` to be approved. A copy of
a project in another folder that claims the same id does not inherit the
approvals.

### Redirects, hooks and token URLs

The check runs on the request as it is finally sent, so a pre-request hook that
rewrites the URL, or a `token_url` on another host, is checked like anything
else. It runs again before every redirect hop. As before, `Authorization` and
`Cookie` are dropped when a redirect changes host, port or scheme. A hop that
continues a request carrying a credential keeps that status while it still
carries anything the project supplied: a body, a header other than `Accept`,
`Accept-Encoding`, `Accept-Language`, `Cache-Control`, `Connection`,
`Content-Type`, `Pragma` or `User-Agent` holding an ordinary value, or a value
from the original query string. Its host must then be allowed and approved, or
the step fails before the hop is sent. A hop that carries none of that, such as
a plain `GET` to a download host, is followed.

### Approving a host

- **Desktop app:** the first time a run needs a host, the app asks, showing the
  project, the host and what would be sent. **Approve and send** remembers the
  answer; **Don't send** (the default) fails that step. Approved hosts are listed
  in **Manage Secrets**, where each can be revoked.
- **Command line at a terminal:** `reqloom run` asks the same question and waits
  for `yes` or `no`. `reqloom hosts approve <host>` approves one ahead of time,
  after asking yes or no.
- **No terminal (an agent, CI, a pipe):** nothing is asked and nothing is
  assumed. The step fails with `E_HOST_NOT_APPROVED` before anything is sent,
  naming the host and how to approve it.
- **CI:** set `REQLOOM_APPROVED_HOSTS` in the pipeline to a comma-separated list
  of hosts, each `[http://|https://]host[:port]`; no scheme means `https`, no
  port the scheme's default (`api.example.com` is `https://api.example.com:443`;
  a local server is `http://127.0.0.1:3000`). It approves them for that run only
  and is never stored. It still respects `allowed_hosts`, and it cannot approve
  TLS verification being off or a project-named certificate authority.

### Letting only the desktop app approve

An AI agent that runs commands may run them in a pseudo-terminal and answer the
prompt, or set `REQLOOM_APPROVED_HOSTS` itself. On a machine where agents run,
turn approval from the command line off: untick **Let the reqloom command line
approve hosts** in **Manage Secrets**, or run `reqloom hosts cli-approval off`.
The terminal prompt, `reqloom hosts approve` and `REQLOOM_APPROVED_HOSTS` then
approve nothing, and only the desktop app can. Turning it back on is possible
only in the app.

This lowers the risk; it does not rule it out. The setting is a file in your
user account, and any program running as you, an agent included, can edit or
delete that file. A pseudo-terminal also looks like a terminal to `reqloom`,
so a program driving one can answer the prompt. Nothing here can prove that a
person answered.

### After upgrading

Projects that ran before host approval existed have no approved hosts. The
first run asks once per host, including the `baseUrl` host. A CI pipeline needs
`REQLOOM_APPROVED_HOSTS` added before its next run. An actor whose sign-in or
`token_url` is on a different host than `baseUrl` is not asked about: it fails
with `E_HOST_NOT_APPROVED` until that host is listed in its `allowed_hosts`.

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
