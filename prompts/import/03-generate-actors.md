# Stage 3 — Generate Actor Files

Input: the Stage 2 schema plan. Output: one YAML file per actor, written to `chainapi/actors/<actor_id>.yaml`. Use your file-writing tools — do NOT dump YAML inline in chat.

## Per-actor file template

For every actor in the plan, produce `chainapi/actors/<actor_id>.yaml` with this exact shape:

```yaml
# Two formats are accepted by the parser. Use the wrapped format below — it
# matches the LLM-generated style and is unambiguous when one file holds one
# actor.
<actor_id>:
  description: <one line>
  auth:
    strategy: simple | chain | api_key | oauth2_client_credentials | oauth2_password | oauth1
    # For 'simple':
    method: POST
    path: /api/v1/auth/login
    headers:
      Content-Type: application/json
    body:
      email: "{{env.<actor>_email}}"
      password: "{{env.<actor>_password}}"
    expect_status: 200
    extract:
      token: $.data.accessToken
      refresh_token: $.data.refreshToken
      user_id: $.data.user.id
    # For 'chain' (e.g. OTP):
    # steps:
    #   - id: send_otp
    #     method: POST
    #     path: /api/v1/auth/send-otp/sms
    #     body: {phone: "{{env.<actor>_phone}}", purpose: login}
    #     expect_status: 200
    #   - id: login
    #     method: POST
    #     path: /api/v1/auth/login
    #     body: {phone: "{{env.<actor>_phone}}", otp: "{{env.<actor>_otp}}"}
    #     expect_status: 200
    #     extract: {token: $.data.accessToken, ...}
  session:
    ttl: 15m
    # If the plan says yes:
    refresh:
      method: POST
      path: /api/v1/auth/refresh
      headers:
        Content-Type: application/json
      body:
        refreshToken: "{{<actor>.refresh_token}}"
      extract:
        token: $.data.accessToken
        refresh_token: $.data.refreshToken
  inject:
    headers:
      Authorization: "Bearer {{<actor>.token}}"
```

## Rules

1. **One file per actor**, named exactly `<actor_id>.yaml`.
2. The top-level key MUST be `<actor_id>:` — the wrapped format. No leading `name:` field.
3. **Don't invent fields.** If the plan didn't say what to put in `extract:`, leave it empty rather than guessing.
4. **Status codes** come from the plan, not from "common knowledge". `200` for login, `201` for register, etc., **only if the plan said so**.
5. The `session.ttl` value is from the plan. Default to `15m` if the plan omitted it (matches a typical short-lived JWT).
6. The `inject.headers` MUST reference variables this actor extracts. If the actor doesn't extract `token`, don't write `Bearer {{<actor>.token}}`.

## What NOT to do

- ❌ Output YAML in chat — write to disk
- ❌ Combine multiple actors in one file
- ❌ Add comments inside the YAML files (they confuse the parser on some YAML libs)
- ❌ Emit fields the plan didn't specify
- ❌ Use `expect_status: 200` as a default — leave it out if the plan didn't say
