# Stage 6 — Fix Lint Errors

Input:
1. The current schema (in `chainapi/`)
2. Output from `chainapi lint --project chainapi`
3. Optionally, output from `chainapi run <op>` if a runtime issue surfaced

Output: edits to existing schema files via your file-writing tools. Do NOT regenerate from scratch.

## Decision tree

For each error, decide which of these patterns it matches and apply the matching fix:

### Schema-time errors (caught by `chainapi lint`)

| Error code | Pattern | Fix |
|---|---|---|
| `E_YAML_PARSE` | YAML syntax error | Open the cited file:line. Fix indentation or quoting. Most common cause: tab-indented inside a space-indented file. |
| `E_REF_UNDEFINED` | Operation references `{{X.y}}` where X is not a known actor/resource/env | Either add the producer (extraction) or change the reference. Check the plan's variable producer/consumer table for the canonical producer of `y`. |
| `E_CYCLE` | A → B → A | Almost always means an op extracts a field its dependency consumes. Break the cycle by moving the extraction to an earlier op or removing a needless `depends_on`. |
| `E_SCHEMA_VERSION` | `version:` outside the supported range (1–3) | Set `version: 1` at the top of `chainapi.yaml`. |

### Runtime errors (caught by `chainapi run`)

| Error code | Likely cause | Fix |
|---|---|---|
| `E_VAR_UNRESOLVED` | A `{{X.y}}` reference has no value at runtime | The producer step didn't run, didn't extract that field, or the field name is wrong. Check the chain summary — was the producer in the chain? Did its `extract:` block name `y` correctly? |
| `E_HTTP_4XX` (with body excerpt showing `field: foo should not exist`) | Schema body has fields the backend doesn't accept | Remove the unwanted field. **Do not add it to a body unless the backend explicitly requires it.** |
| `E_HTTP_4XX` (with body excerpt showing `field: foo is required`) | Schema body is missing a required field | Add the field. If you don't have a value, mark it as a `{{env.X}}` variable so the user can supply it. |
| `E_HTTP_4XX` (with body excerpt showing `Invalid OTP` or similar) | The op needs a prerequisite call (send OTP, fetch CSRF, etc.) | Add `depends_on: [<prereq_op>]`. If the prereq op doesn't exist yet, create it. |
| `E_HTTP_4XX` (with body showing `Phone Already Registered` or similar uniqueness collision) | Hardcoded a value that must be unique per run | Replace the hardcoded value with `{{env.X}}` where X is marked as a per-run override variable. |
| `E_STATUS_MISMATCH` (expected 200, got 201) | Wrong `expect_status` | Update `expect_status:` to match what the backend actually returns. |
| `E_EXTRACTION_FAILED` | Extraction's JSONPath doesn't match the response | Look at the response body excerpt (the engine logs it on failure). Fix the path: `$.data.id`, `$.data[0].id`, `$.user.id`, etc. |

## Rules

1. **Don't regenerate from scratch.** Stage 1–5 already cost time. Make minimal edits.
2. **Read the failing op's section** of the plan before editing. If the plan was wrong, fix the plan in chat first, then apply the fix.
3. **Test one fix at a time.** Run `chainapi lint` after every edit. For runtime fixes, run the op via `chainapi run` to confirm.
4. **Document fixes in the plan.** If you discover the API actually requires `{phone+OTP}` not `{phone+password}`, update the plan's auth flow section so the next iteration matches.
5. **If the response contradicts the plan**, the plan is wrong. Update it. The plan is the source of truth — schema files derive from it.

## When to give up and ask the human

After three iterations on the same error, stop and ask. Likely causes:

- The API documentation is wrong or outdated
- The endpoint has authorization rules the docs don't mention
- The endpoint requires server-side state (a real organization, a verified user) that doesn't exist yet
- The body shape uses something exotic (multipart, custom encoding, signed payloads) that needs a `pre_request` hook

These all need human intervention. Don't keep guessing.
