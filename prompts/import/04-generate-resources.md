# Stage 4 — Generate One Resource File

Input: the Stage 2 schema plan + the name of one specific resource. Output: `chainapi/resources/<resource_id>.yaml`. Use your file-writing tools — do NOT dump YAML inline in chat.

You will be invoked once per resource. Generate ONLY the resource you were told to generate. Do not generate other resources.

## File template

```yaml
<resource_id>:
  description: <one line from the plan>
  operations:
    <op_name>:
      method: GET | POST | PUT | PATCH | DELETE
      path: /api/v1/foo/{{<resource>.<id_field>}}
      actor: <actor_id>          # omit this line entirely if the op is public
      depends_on: [<other_resource>.<op_name>]   # omit this line if no deps
      headers:
        Content-Type: application/json           # only when there's a body
      query_params:                              # omit entire block if no params
        page: "1"
        limit: "20"
      body:                                      # omit if no body
        field1: "<from plan>"
        field2: "{{$.uuid}}"
      expect_status: 201
      extract:                                   # omit if no extractions
        new_id: $.data.id

    # Repeat for every operation
```

## Rules

1. **One file, one resource.** Don't generate any other resource even if the plan touches them.
2. **Top-level key is `<resource_id>:`** — wrapped format.
3. **Operation names** match the plan exactly. Do not pluralize, abbreviate, or expand them.
4. **`actor:` is omitted entirely for public endpoints.** Don't write `actor: public` or `actor: ""` — just leave the field out.
5. **`depends_on:` is omitted when there are no deps.** Don't write `depends_on: []`.
6. **Body fields:**
   - Required fields from the plan: include
   - Optional fields from the plan: include only if they have a clear value source
   - Unknown fields from the plan: **OMIT** — let the schema fail at runtime if needed; don't poison the body with hallucinated fields
7. **Variable references:**
   - For path parameters that come from another op's response: `{{<that_resource>.<extract_name>}}`
   - For env-supplied values: `{{env.<key>}}`
   - For unique-per-run values like idempotency keys: `{{$.uuid}}`
   - **Never reference an actor's session var that the actor doesn't produce.** If `admin_organization.verify` needs the new org's ID, it should `depends_on: [auth.register_employer]` and reference `{{auth.employer_org_id}}` — NOT `{{employer.org_id}}` (which would require the employer actor to be active, which it isn't when admin runs verify).
8. **`extract:` produces a flat name → JSONPath map.** Names should match what the plan's variable producer/consumer table says. JSONPath uses `$.data.field` or `$.data[0].field` (array indices supported).
9. **`expect_status:` from the plan.** If the plan said `<unknown>`, leave it out — better to silently accept any 2xx than to fail on a wrong guess.

## What NOT to do

- ❌ Generate multiple resources in one call
- ❌ Output YAML in chat — write to disk
- ❌ Add comments inside the YAML
- ❌ Use `actor: public` or `depends_on: []` — omit those fields entirely
- ❌ Hallucinate body fields the plan didn't specify
- ❌ Cross-reference actor session vars that the actor doesn't extract
- ❌ Default `expect_status` to 200 — leave it unset if unknown

## Examples of good vs bad

### Good
```yaml
admin_organization:
  description: Admin verifies new organizations
  operations:
    verify:
      method: PATCH
      path: /api/v1/admin/organizations/{{auth.employer_org_id}}/verify
      actor: admin
      depends_on: [auth.register_employer]
      headers:
        Content-Type: application/json
      body:
        status: verified
      expect_status: 200
```

### Bad — references session var employer doesn't produce
```yaml
admin_organization:
  operations:
    verify:
      method: PATCH
      path: /api/v1/admin/organizations/{{employer.org_id}}/verify   # ❌
      actor: admin
      body:
        status: approved
        notes: "All documents verified"   # ❌ hallucinated field — backend rejects
```
