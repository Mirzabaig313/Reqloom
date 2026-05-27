# Stage 5 — Generate Environment File

Input: the Stage 2 plan's "Environment variables required" table. Output: `chainapi/environments/local.yaml`. Use your file-writing tools — do NOT dump YAML inline in chat.

## File template

The parser accepts both flat-key and `name:`/`variables:` wrapped formats. Use the **flat-key** format below — it's what the LLM-generated style produces and matches what most users hand-write:

```yaml
# chainapi/environments/local.yaml
baseUrl: http://localhost:3000
admin_email: admin@example.com
admin_password: <REPLACE_WITH_REAL_PASSWORD>
employer_email: <REPLACE_OR_REGISTER_VIA_auth.register_employer>
employer_password: <SET_AT_REGISTRATION>
new_employer_phone: <REPLACE_PER_RUN_VIA_--var>
worker_phone: <REPLACE_OR_REGISTER_VIA_auth.register_employee>
worker_otp: "123456"
new_worker_phone: <REPLACE_PER_RUN_VIA_--var>
```

## Rules

1. **Every env variable in the plan's table** appears in this file.
2. **Real credentials:** if the plan says the API has hardcoded test users (admin, demo employer, etc.), use them. Otherwise use the placeholder `<REPLACE_WITH_REAL_VALUE>`.
3. **Per-run override variables** — the plan marks these with "per-run override? yes". Set their values to a recognizable placeholder like `<REPLACE_PER_RUN_VIA_--var>`. The user passes real values via `chainapi run X --var key=value`.
4. **Sensitive values:** if the plan marks a variable as `secret`, write the value as `!secret <KEY_NAME>` (the keychain-backed transformer). For example: `admin_password: !secret ADMIN_PASSWORD`. The user populates the keychain separately.
5. **Comments:** add **one** top-of-file comment explaining how to fill in placeholders. No inline comments per variable.
6. **`baseUrl`** should NOT include `/api/v1` or any version prefix — paths in operations include the version.

## Self-check

- [ ] Does every variable referenced as `{{env.X}}` in any actor or resource appear here?
- [ ] Are all per-run override variables clearly marked as placeholders, not real values?
- [ ] Does `baseUrl` end at the host (`http://localhost:3000`), not the version (`/api/v1`)?
- [ ] Are sensitive credentials marked with `!secret` if appropriate?

## Output

Write the file to `chainapi/environments/local.yaml`. Reply with a one-line summary: how many variables were generated, and which ones need user attention before the first run.
