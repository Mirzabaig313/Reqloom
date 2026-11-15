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

`ttl` accepts `ms`, `s`, `m`, `h`, `d`, and a bare number is whole seconds —
`ttl: 3600` is one hour. Default is 15m. The session is kept in whole seconds, so
`ttl: 900ms` counts as 0. A malformed value such as `ttl: 15 minutes` fails the
load:

```ansi
LINT FAIL [E_SCHEMA_INVALID]: ./actors/vendor.yaml:9:8: 'ttl' must be a duration such as 500ms, 5s, 1m, 1h or 1d (a bare number is seconds), got '15 minutes'
```

For the OAuth 2 strategies, the token response's `expires_in` also counts: the
session ends 30 seconds before the token expires, or at `ttl` if that is
sooner.

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

The next request sends the new credential. `inject.headers` templates such as
`Bearer {{vendor.token}}` read the merged values. For the `bearer`, `api_key`,
and OAuth 2 strategies, which build their header themselves, the refresh must
extract `token`, `key`, or `access_token` respectively to replace it; otherwise
the old credential is kept.

With no `expect_status`, any 2xx counts as success.

If the refresh fails, the engine falls back to a full login. If that fails too,
the step fails with `E_SESSION_REFRESH_FAILED`. The same code appears when a
first *login* fails — including when the server is simply unreachable:

```ansi
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
```

So this code means "couldn't establish this actor's identity", not necessarily
"the refresh endpoint is broken". The detail says why — it starts with
`actor 'vendor' could not sign in:` and gives the cause, such as the status and
the login response with secrets masked, or the network error. Check the server
is up and the credentials are right.

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

The desktop app keeps sessions, cookies, and cached extracted values **per
environment**. Switching from `staging` to `local` never reuses the staging
login or a staging order id; switching back picks up where the staging context
left off. **Run cleanly** and **Reset caches** clear the active environment's
context, including cached OAuth 2 tokens for inline (actor-less) auth.

Session values are **not** written to your project directory, so there's nothing
to gitignore and no token to accidentally commit.

## Cookies

Each actor has its own cookie jar. A `Set-Cookie` on any of that actor's
responses — the login, an operation, a poll — stores the cookie with the host
that set it and its `Domain`, `Path`, `Secure` and expiry. A cookie is sent only
where it belongs: to that host (or the hosts its `Domain` covers), on a matching
path, over https if it is `Secure`, and not after it expires. A response that
deletes a cookie — `Max-Age` of 0 or less, an `Expires` in the past, or an empty
value — removes it, on any response, not only a logout.

Two limits:

- A cookie set by a response after a redirect is stored under the host of the
  original request, not the host that was redirected to.
- A cookie without `Path` is sent on every path of its host.

## Secrets are loaded once per run

Related but distinct: at the start of a real run, the engine collects the
`{{secret.X}}` names your schema actually references and reads exactly those from
the keychain. Not a bulk dump, and not re-read per step.

Dry runs — `reqloom lint`, and the desktop's chain preview — **skip secret
loading entirely**, so they never trigger a keychain unlock prompt. Unresolved
`{{secret.X}}` in a preview is expected. See
[secrets](/schema/secrets-and-transport/#secrets).

:::tip[Session state at a glance]
The [desktop app](/desktop/overview/) shows a dot next to each actor in the
Explorer for whether its session is live, expiring, or absent — and offers
**Run cleanly** and **Reset caches** to discard cached sessions and extractions
without editing anything. See
[fresh sessions](/desktop/running/#fresh-sessions).
:::

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
