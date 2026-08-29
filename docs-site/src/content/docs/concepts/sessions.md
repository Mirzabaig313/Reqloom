---
title: Sessions & caching
description: "Why an actor logs in once instead of once per request: session TTL, token refresh, and what gets reused inside a run."
---

Two layers of reuse keep a run from repeating work: **sessions** cache an actor's
credentials, and **step results** cache values within a run.

## Sessions

When an actor is first needed, its `auth` block runs and the extracted values
become that actor's session:

```yaml title="actors/vendor.yaml"
name: vendor
auth:
  strategy: simple
  path: /api/v1/auth/vendor/login
  extract:
    token: $.data.accessToken
    refresh_token: $.data.refreshToken
    vendor_id: $.data.user.id

session:
  ttl: 15m
```

For the next 15 minutes that session is reused. A seven-step chain touching three
actors performs three logins, not seven — and logins never appear as chain steps,
because each one happens inside the step that needed it.

`ttl` accepts `s`, `m`, `h`, `d`. Default is 15m.

:::caution[A malformed TTL becomes 15m silently]
`ttl: 15 minutes` is not valid and falls back to the default with no warning.
Write `15m`.
:::

### Choosing a TTL

Match your API's token lifetime, slightly under it:

| Token lifetime | Reasonable `ttl` |
| --- | --- |
| 1 hour | `45m` |
| 15 minutes | `10m` |
| Long-lived / static | `1h` or more |

Too long and you'll send expired tokens and get 401s. Too short and you'll
re-authenticate needlessly — which matters if your login endpoint is rate
limited.

## Refresh instead of re-login

When a session expires, a `refresh` block avoids a full re-authentication:

```yaml
session:
  ttl: 15m
  refresh:
    method: POST
    path: /api/v1/auth/refresh
    body:
      refresh_token: "{{vendor.refresh_token}}"
    extract:
      token: $.data.accessToken
```

One request instead of the whole login chain — worth it when login is expensive
(OTP, MFA) or rate limited.

Refresh extractions are **merged** into the existing session rather than
replacing it. Above, `token` is updated while `vendor_id` and `refresh_token`
survive, so `{{vendor.vendor_id}}` keeps working. That merge is why you only need
to extract what actually changed.

With no `expect_status`, any 2xx counts as success.

If refresh fails, the step fails with `E_SESSION_REFRESH_FAILED`. The same code
appears when a *login* fails — including when the server is simply unreachable:

```ansi
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
```

So this code means "couldn't establish this actor's identity", not necessarily
"the refresh endpoint is broken". Check the server is up and the credentials are
right.

## Step caching within a run

Separately from sessions, the engine won't repeat an operation whose values it
already has. If two branches both need `product.create`, it runs once and the
second appearance is skipped:

```ansi
  OK     product.create (113ms) err=—
  SK     product.create (0ms) err=—
```

That's what keeps a wide graph from re-creating the same fixture repeatedly.

### Forcing a re-run

When each occurrence must create fresh data:

```yaml
  create:
    method: POST
    path: /api/v1/orders
    force: true
```

`force: true` opts that operation out of caching — it executes every time it
appears in a chain.

## Where sessions live

In memory, for the lifetime of the process. A new `reqloom run` starts with no
sessions and authenticates again, so the TTL matters within a run — and in the
desktop app, across the operations you click during a working session.

Session values are **not** written to your project directory, so there's nothing
to gitignore and no token to accidentally commit.

## Secrets are loaded once per run

Related but distinct: at the start of a real run, the engine collects the
`{{secret.X}}` names your schema actually references and reads exactly those from
the keychain. Not a bulk dump, and not re-read per step.

Dry runs — `reqloom lint`, and the desktop's chain preview — **skip secret
loading entirely**, so they never trigger a keychain unlock prompt. Unresolved
`{{secret.X}}` in a preview is expected. See
[secrets](/schema/secrets-and-transport/#secrets).

## Debugging session problems

**Getting 401s partway through a long run.** The token expired mid-run. Lower
`ttl`, or add a `refresh` block.

**Every run re-authenticates.** Expected — sessions don't persist between CLI
invocations. If login is rate limited, consider a longer-lived credential in the
environment for CI rather than a login chain.

**`E_SESSION_REFRESH_FAILED` on the first step.** Usually not a session problem
at all: the login request itself failed. Check `baseUrl`, that the server is
running, and the credentials.

**Values from login are missing.** Confirm the auth `extract` paths match the real
response shape. An actor variable that was never extracted resolves to nothing and
shows up as `E_VAR_UNRESOLVED` in whatever used it.

## Next

- [Auth strategies](/schema/auth-strategies/) — the eleven ways to log in
- [Actors](/concepts/actors/) — what a session belongs to
- [Error codes](/reference/error-codes/) — `E_SESSION_REFRESH_FAILED` and friends
