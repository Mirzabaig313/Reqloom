---
title: Auth strategies
description: "All eleven actor auth strategies with their exact keys, plus per-operation inline auth for one-off endpoints."
---

An actor's `auth:` block says how to obtain credentials; `inject:` says how to
attach them to every request that actor makes. Pick a strategy by how your API
issues credentials.

| Strategy | Use when | Network call |
| --- | --- | --- |
| [`simple`](#simple) | One login request returns a token | yes |
| [`chain`](#chain) | Login takes several steps — OTP, MFA, tenant select | yes |
| [`basic`](#basic) | HTTP Basic | no |
| [`api_key`](#api_key) | A static key in a header, query param, or cookie | no |
| [`bearer`](#bearer) | You already have a token | no |
| [`oauth2_client_credentials`](#oauth2_client_credentials) | Machine-to-machine OAuth 2 | yes |
| [`oauth2_password`](#oauth2_password) | OAuth 2 resource-owner password grant | yes |
| [`oauth1`](#oauth1) | OAuth 1.0a, HMAC-SHA1 signed | no |
| [`aws_sigv4`](#aws_sigv4) | AWS Signature v4 | no |
| [`jwt`](#jwt) | You sign your own JWT | no |
| [`mtls`](#mtls) | Client-certificate TLS | no |

`strategy:` defaults to `simple`. An unrecognised value **silently falls back to
`simple`** — note `oauth2` alone is not valid, it's `oauth2_client_credentials`.

## simple

One request, extract a token. The most common shape.

```yaml title="actors/vendor.yaml"
name: vendor
description: Marketplace vendor with email/password auth

auth:
  strategy: simple
  method: POST                      # default POST
  path: /api/v1/auth/vendor/login
  body:
    email: "{{env.vendor_email}}"
    password: "{{env.vendor_password}}"
  expect_status: 200
  extract:
    token: $.data.accessToken
    refresh_token: $.data.refreshToken
    vendor_id: $.data.user.id

session:
  ttl: 15m

inject:
  headers:
    Authorization: "Bearer {{vendor.token}}"
```

Everything `extract` produces becomes an actor-scoped variable — `{{vendor.token}}`,
`{{vendor.vendor_id}}` — usable anywhere, not just in `inject`.

Keys: `method`, `path`, `headers`, `body`, `expect_status` (scalar only here),
`extract`.

## chain

Several requests in order. Each step can use values the previous one extracted.

```yaml title="actors/customer.yaml"
name: customer
auth:
  strategy: chain
  steps:
    - id: request_otp
      method: POST
      path: /api/v1/auth/otp/request
      body:
        phone: "{{env.test_phone}}"
      expect_status: 200
    - id: verify_otp
      method: POST
      path: /api/v1/auth/otp/verify
      body:
        phone: "{{env.test_phone}}"
        code: "000000"
      expect_status: 200
      extract:
        token: $.data.accessToken
        customer_id: $.data.user.id

inject:
  headers:
    Authorization: "Bearer {{customer.token}}"
```

Per-step keys: `id`, `method`, `path`, `headers`, `body`, `expect_status`,
`extract`. `steps` must be a sequence.

## basic

No network call — the header is computed.

```yaml
auth:
  strategy: basic
  username: "{{env.api_user}}"
  password: "{{secret.API_PASSWORD}}"
```

No `inject:` needed; the `Authorization: Basic …` header is added for you.

## api_key

```yaml
auth:
  strategy: api_key
  key: "{{secret.API_KEY}}"
  location: header                  # header | query | cookie
  name: X-API-Key
```

`key` is the value. `location` and `name` are optional.

## bearer

A token you already hold — from CI, or another actor.

```yaml
auth:
  strategy: bearer
  token: "{{secret.SERVICE_TOKEN}}"
```

## oauth2_client_credentials

RFC 6749 §4.4. Reqloom fetches the token and injects it.

```yaml
auth:
  strategy: oauth2_client_credentials
  token_url: https://auth.example.com/oauth/token
  client_id: "{{env.client_id}}"
  client_secret: "{{secret.CLIENT_SECRET}}"
  scope: "orders:read orders:write"     # optional
```

## oauth2_password

RFC 6749 §4.3 — the above plus user credentials.

```yaml
auth:
  strategy: oauth2_password
  token_url: https://auth.example.com/oauth/token
  client_id: "{{env.client_id}}"
  client_secret: "{{secret.CLIENT_SECRET}}"
  username: "{{env.user_email}}"
  password: "{{secret.USER_PASSWORD}}"
  scope: "profile"                      # optional
```

:::note[Authorization-code grant is desktop-only]
The interactive browser redirect can't run in a headless engine. The CLI supports
the non-interactive grants above; the desktop app handles the authorization-code
flow with PKCE and holds the resulting token in memory.
:::

## oauth1

RFC 5849, HMAC-SHA1, signed per request.

```yaml
auth:
  strategy: oauth1
  consumer_key: "{{env.consumer_key}}"
  consumer_secret: "{{secret.CONSUMER_SECRET}}"
  token: "{{env.oauth_token}}"          # optional
  token_secret: "{{secret.TOKEN_SECRET}}"   # optional
  realm: "Example"                      # optional
```

## aws_sigv4

Signed per request with AWS Signature v4.

```yaml
auth:
  strategy: aws_sigv4
  access_key: "{{env.AWS_ACCESS_KEY_ID}}"
  secret_key: "{{secret.AWS_SECRET_ACCESS_KEY}}"
  region: us-east-1
  service: execute-api
  session_token: "{{secret.AWS_SESSION_TOKEN}}"   # optional, for STS
```

## jwt

Sign your own JWT and send it as a bearer token.

```yaml
auth:
  strategy: jwt
  secret: "{{secret.JWT_SIGNING_KEY}}"
  algorithm: HS256                  # HS256 (default) or HS512
  payload: '{"sub":"user-123","role":"admin"}'
```

`payload` is a JSON object **as a string**.

## mtls

Client-certificate TLS. Applies at the transport layer.

```yaml
auth:
  strategy: mtls
  format: pem                       # pem (default) or p12
  cert_path: /etc/certs/client.pem
  key_path: /etc/certs/client-key.pem
  key_password: "{{secret.KEY_PASSPHRASE}}"   # optional
  ca_cert_path: /etc/certs/ca.pem             # optional
```

Certificate paths are **not** sandboxed to the project root — client certs
usually live outside the repo. Prefer absolute paths.

## Sessions and refresh

A session is cached for `ttl` (default 15m) so you don't re-authenticate on every
run. When it expires, a `refresh` block avoids a full re-login:

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

Refresh extractions are **merged** into the existing session, so variables the
login produced and refresh doesn't (like `vendor_id`) survive. With no
`expect_status`, any 2xx counts as success.

Malformed `ttl` values fall back to 15m silently — `ttl: 15 minutes` is not
valid. See [sessions & caching](/concepts/sessions/).

## Inline auth for one endpoint

For a single endpoint that doesn't justify an actor — a third-party callback, a
health check on another host — put the credential on the operation:

```yaml
  check_partner:
    method: GET
    path: /partner/status
    auth:
      type: bearer
      token: "{{secret.PARTNER_TOKEN}}"
```

`type:` accepts `bearer`, `basic`, `apikey` (or `api_key`), `aws_sigv4`,
`oauth1`, `oauth2`, `jwt`, `mtls`, and `inherit`. Anything unrecognised means
**no auth is applied** — silently.

Common shapes:

```yaml
    auth:
      type: basic
      username: admin
      password: "{{secret.ADMIN_PW}}"
```

```yaml
    auth:
      type: apikey
      key: X-API-Key
      value: "{{secret.API_KEY}}"
      in: header              # `query` puts it in the query string instead
```

`type: inherit` uses the project-wide default declared at the root:

```yaml title="reqloom.yaml"
auth:
  type: bearer
  token: "{{secret.DEFAULT_TOKEN}}"
```

If an operation sets both `actor:` and `auth:`, inline auth is applied last and
wins on the `Authorization` header.

## Choosing between actor and inline auth

Use an **actor** when the credential is an identity you'll reuse — it gets
session caching, refresh, and one place to change the password. Use **inline
auth** for a one-off endpoint where standing up a login chain is more work than
the test is worth.

## Next

- [Actors](/concepts/actors/) — the concept and when to add another one
- [Sessions & caching](/concepts/sessions/) — TTL, refresh, and forcing re-auth
- [Secrets](/schema/secrets-and-transport/#secrets) — keeping credentials out of git
