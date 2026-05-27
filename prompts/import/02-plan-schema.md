# Stage 2 — Plan the Schema

You are designing a ChainAPI schema. Input: the Stage 1 digest plus any human corrections. Output: a written plan that a developer can review in 5 minutes before any YAML is generated. Do NOT generate YAML in this stage.

## Output format

Return Markdown with exactly these sections:

```markdown
## Project metadata
- name: <api name>
- default_environment: local
- description: <one line>

## Actors

### actor: <actor_id>
- description: <who>
- auth strategy: simple | chain | api_key | oauth2_client_credentials | oauth2_password | oauth1
- auth steps:
  1. POST /api/v1/auth/login
     body: {email: "{{env.<actor>_email}}", password: "{{env.<actor>_password}}"}
     extract: {token: $.data.accessToken, refresh_token: $.data.refreshToken, user_id: $.data.user.id}
     expect_status: 200
- session ttl: 15m | 1h | 24h
- session refresh: <yes/no, with body shape>
- inject headers: {Authorization: "Bearer {{<actor>.token}}"}
- environment variables this actor needs: <actor>_email, <actor>_password

(Repeat for every actor)

## Resources

### resource: <resource_id>

#### Operation: <resource>.<op_name>
- method: POST
- path: /api/v1/foo
- actor: <actor_id>  (or "public" if no auth)
- depends_on: [<other_resource>.<op>] — explicit prereqs
- body fields (from docs):
  - foo: required, "{{env.test_foo}}"
  - bar: optional, "{{$.uuid}}"
- expect_status: 201
- extracts:
  - foo_id: $.data.id (used by: <r>.<op>, <r>.<op>)

(Repeat for every operation)

## Variable producer/consumer table

| variable | produced by | consumed by |
|----------|-------------|-------------|
| auth.employer_org_id | auth.register_employer | admin_organization.verify |
| order.order_id | order.create | order.pay, order.get, refund.request |

## Environment variables required

List every `{{env.X}}` reference and what kind of value it expects:

| variable | description | placeholder | per-run override? |
|----------|-------------|-------------|-------------------|
| baseUrl | API base URL | http://localhost:3000 | no |
| admin_email | admin login email | admin@example.com | no |
| new_employer_phone | unique phone for registration | "" (must be set per run) | yes — `--var` |

## Sanity checks (you must verify)

- [ ] Every `{{<scope>.<field>}}` reference has a producer in the variable table
- [ ] Every operation's `actor` is defined in Actors above
- [ ] Every operation in a `depends_on` list is defined in Resources
- [ ] Every actor with `session.refresh` has a refresh token in its extract
- [ ] Every operation that mutates state (POST/PUT/PATCH/DELETE) has explicit `expect_status`
- [ ] Every body field is marked required/optional with a value source
- [ ] No actor produces session vars that no operation consumes (dead extractions)
- [ ] No operation references session vars its actor doesn't produce
```

## Rules

1. **Variable producer/consumer table is mandatory.** Every `{{X.y}}` reference in any path/body/header MUST appear in this table with a producer. If a producer doesn't exist, the reference is a bug — flag it.
2. **Be explicit about per-run overrides.** Phone numbers, emails, and other values that must be unique per run go to env vars marked "per-run override? yes". Do NOT hardcode them in op bodies.
3. **Cross-actor references** — if `admin_organization.verify` needs `org_id` from a fresh registration, it must `depends_on: [auth.register_employer]` and the `extract` block on `register_employer` must produce `employer_org_id`. The verify op then uses `{{auth.employer_org_id}}`, not `{{employer.org_id}}` (the employer actor session doesn't carry that ID).
4. **Auth steps follow the actual API.** If the docs say `POST /send-otp` is required before `POST /register/employee`, **that prerequisite belongs in the resource's `depends_on`**, not silently omitted.
5. **Explicit assumptions.** If the digest didn't tell you the response status of an op, write `expect_status: <unknown — verify against running API>` and the human will fix it.

## Self-check before you respond

- [ ] Did you re-read the digest's "Open questions" section and address every one?
- [ ] Does every `{{X.y}}` reference appear in the producer/consumer table with a real producer?
- [ ] Are there any cross-actor dependencies (one actor's data being consumed by another actor's operation) that need `depends_on` rather than session-var references?
- [ ] Have you marked unique-per-run values as env vars with `per-run override? yes`?
- [ ] Did you list operations the user might want that **aren't possible to express in this schema** (async polling, websockets, file uploads) under "Out of scope"?

The human reads this plan in 5 minutes. If they catch a problem, the fix is one paragraph. Do NOT skip this stage to save time.
