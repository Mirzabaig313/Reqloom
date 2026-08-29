---
title: Multi-stage prompt suite
description: "Six prompts that turn an API description into a working schema, and why splitting the work beats one big prompt."
---

The prompt suite lives in
[`prompts/import/`](https://github.com/Mirzabaig313/Reqloom/tree/main/prompts/import).
It's plain Markdown — paste each stage into whichever model you use. No API key
required, no integration to configure.

## The six stages

| Stage | Prompt | Input | Output | Calls |
| --- | --- | --- | --- | --- |
| 1. Discover | `01-discover.md` | OpenAPI / Markdown / curl logs | A structured digest of actors, resources, operations, inferred chains | 1 |
| 2. Plan | `02-plan-schema.md` | Stage 1 digest + your corrections | A plain-English schema plan | 1 |
| 3. Actors | `03-generate-actors.md` | Stage 2 plan | YAML for every actor | 1 |
| 4. Resources | `04-generate-resources.md` | Stage 2 plan + actor names | YAML per resource, one call each | N |
| 5. Environment | `05-generate-environment.md` | Stage 2 plan | `environments/local.yaml` with placeholders | 1 |
| 6. Fix | `06-fix-lint-errors.md` | `reqloom lint` output + failing files | Patched files | iterative |

There's also `system.md`, a short system prompt to set alongside each stage, and a
`few-shot/` directory for examples.

For a 50-endpoint API expect **8–15 calls**, roughly **$0.10–$0.40** on Claude
Sonnet or GPT-4o.

## Why not one prompt

This is the part worth internalising. The first single-prompt attempt against a
real backend produced a schema that was syntactically valid and **broke on contact
in five different ways**. Three failure modes drove the split:

**Hallucinated detail.** Asked for YAML directly, a model fills gaps with
plausible guesses — `expect_status: 200` on an endpoint that returns `201`, a
`body` with invented field names. Because the output *looks* right, the errors
survive review. Stage 2 forces the plan into plain English first, where a wrong
assumption is obvious in a sentence rather than buried in 40 lines of YAML.

**Lost context on long output.** Generating 30 resource files in one response
triggers truncation, repetition, or silently dropped fields. Stage 4 is N calls of
one resource each, so no single response is long enough to degrade.

**No review gate.** In a single prompt, by the time you see anything you've spent
the whole budget. The Stage 2 plan is readable in five minutes and cheap to
regenerate.

## The Stage 2 gate matters most

Stage 2 is where you earn the time back. Read the plan and check four things:

1. **Are the actors right?** One per identity the API recognises, not one per
   token. See [actors](/concepts/actors/#one-actor-per-identity-not-per-credential).
2. **Are the resource boundaries right?** Following the API's nouns, and split
   where lifecycles genuinely differ.
3. **Are the dependency edges real?** This is where models guess most. An edge that
   doesn't exist creates a chain that can't run; a missing one creates a request
   with no prerequisites.
4. **Did it invent anything?** Any status code, field name, or path not present in
   your input is a guess. Mark those for verification.

Correcting the plan costs one paragraph. Correcting the generated YAML costs a
round through every affected file.

## Stage 6 closes the loop

`reqloom lint` output is designed to be pasted straight back at a model — it names
the operation and the error code:

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'order.get' references undefined symbol
'invoice.invoice_id': no actor, resource, env, or secret named 'invoice'
```

Iterate until lint is clean:

```bash
reqloom lint --project my-api
```

:::caution[Lint clean is not the same as correct]
Lint validates structure. It cannot tell you whether a path, body, or
`expect_status` matches the real API, and it
[validates reference scopes rather than fields](/cli/lint/#what-it-does-not-check)
— so `{{order.typo}}` passes. A generated schema still needs one real run per chain
you care about.
:::

## Mark what was guessed

Generated operations carry `_provenance`, which the runtime ignores but you
shouldn't:

```yaml
    _provenance:
      source: ai_import
      verified_against: synthetic
      model: claude-sonnet-4
      imported_at: 2026-08-29T08:15:47Z
      evidence:
        actor: "inferred from the /admin path prefix"
        extract.order_id: "guessed from a response example"
```

`verified_against: synthetic` means the model made up the sample response — the
weakest signal there is. Once you've seen a real response, that becomes
`live_capture`. It's how you keep track of which of 105 operations you've actually
confirmed.

## Cost control

- Run stages 1–2 on your strongest model; the plan is where quality pays off
- Stage 4 is repetitive and cheaper on a smaller model
- Cache the Stage 1 digest — you'll re-run stage 2 more than once
- Don't import all 200 endpoints. Import the resources whose chains you'll test

## Licence

The prompt suite is a designated **paid component**, not covered by the Apache 2.0
licence over the engine, CLI, schema, and desktop app. See
[`LICENSE`](https://github.com/Mirzabaig313/Reqloom/blob/main/LICENSE).

## Next

- [Overview & playbook](/ai-importer/playbook/) — the end-to-end workflow
- [Importing OpenAPI](/ai-importer/openapi/) — when to skip the LLM entirely
- [Importing curl logs](/ai-importer/curl/) — the hardest input
