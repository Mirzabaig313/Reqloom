# Stage 1 — Discover

You are reading API documentation. Your only job is to produce a structured digest. Do NOT generate ChainAPI YAML in this stage. Do NOT invent detail.

## Output format

Return Markdown with exactly these sections:

```markdown
## Actors
- <actor_id>: <one line — who they are, what auth they use, what permissions>

## Resources
- <resource_id>: <one line — what domain entity, who creates it, who can mutate it>

## Operations
| operation | method | path | actor | required body fields | response body shape | status code | notes |
|-----------|--------|------|-------|---------------------|---------------------|-------------|-------|
| <r>.<op> | POST | /api/v1/foo | admin | name, email | { data: { id, ... } } | 201 | … |

## Inferred chains
For every operation that requires another operation's output, list the chain:
- <r>.<op> requires: [<r>.<op>, <r>.<op>] because: <reason>

## Auth flows
For every actor, describe the auth sequence:
- <actor>: POST /login {email, password} → returns {accessToken, refreshToken, user.id}
- <actor> (chain): POST /send-otp {phone} → POST /verify-otp {phone, otp} → returns {accessToken}

## Open questions
List anything you couldn't determine from the input. These are mandatory — even if you can guess, write the guess as an open question so the human reviews it.
- "Is the worker registration endpoint expecting OTP first or does the dev environment auto-bypass?"
- "What HTTP status does PATCH /admin/orgs/{id}/verify return on success — 200 or 204?"
```

## Rules

1. **Never invent paths, fields, or status codes.** If the input doesn't say, mark it in "Open questions".
2. **Every operation row** must have all eight columns filled OR the missing ones must appear in "Open questions" referencing the operation by name.
3. **Inferred chains** must cite the reason. "Path contains `{order_id}`" is a valid reason. "Probably needs an order first" is not — that's an open question.
4. **Auth flows** must show the exact JSON path of the extracted token. If the input doesn't show a sample response, mark it as an open question.
5. Output is Markdown only. No code blocks except for sample request/response if absolutely necessary for clarity.

## Self-check before you respond

- [ ] Every operation has a method, path, and status code or appears in "Open questions"?
- [ ] Every chain reason is grounded in the input docs (not inference)?
- [ ] Every auth flow includes the JSONPath for the extracted token?
- [ ] If you weren't sure about anything, did you list it in "Open questions"?

If any answer is no, fix the digest before responding.
