# ChainAPI AI Importer — Prompt Suite

> Stop trying to do AI import in one prompt. The first attempt against GiGwala produced a syntactically valid schema that broke on contact with the real backend in five different ways. This suite splits the work across stages, each with its own prompt and review gate.

## When to use which prompt

| Stage | Prompt | Input | Output | Time |
|---|---|---|---|---|
| 1. Discover | `01-discover.md` | OpenAPI / Markdown / curls | A **structured digest** of actors, resources, ops, and inferred chains | 1 LLM call |
| 2. Plan | `02-plan-schema.md` | Stage 1 digest + your hints | A **schema plan** in plain English describing every actor, resource, op, and dependency edge | 1 LLM call |
| 3. Generate actors | `03-generate-actors.md` | Stage 2 plan | YAML files for every actor | 1 LLM call |
| 4. Generate resources | `04-generate-resources.md` | Stage 2 plan + actor names | YAML files for every resource (one batch per resource — N calls) | N LLM calls |
| 5. Generate environment | `05-generate-environment.md` | Stage 2 plan | The `local.yaml` env file with placeholders for credentials | 1 LLM call |
| 6. Lint and fix | `06-fix-lint-errors.md` | `chainapi lint` output + the failing schema | Patched files | iterative |

Total: typically 8–15 LLM calls for a 50-endpoint API. Cost: ~$0.10–$0.40 with Claude Sonnet or GPT-4o.

## Why multi-stage beats single-prompt

The single-prompt approach has three failure modes:

1. **Hallucinated detail** — the model fills gaps with plausible-looking guesses (`expect_status: 200`, `body: { status: approved }`) instead of asking. Multi-stage forces the model to write the plan in plain English first, where wrong assumptions are easy to spot before YAML is generated.
2. **Lost context on long output** — generating 30 resource files in one response triggers truncation, hallucinated repetition, or the model dropping fields. Stage 4 is N calls of one resource each.
3. **No review gate** — by the time you see the YAML, you've burned the whole budget. The Stage 2 plan is human-readable in five minutes.

## Workflow

```
input docs (OpenAPI / Markdown / curls)
        ↓
   [Stage 1] DIGEST  ──── you spot-check actor + resource list
        ↓
   [Stage 2] PLAN  ────── you read the plan and correct the model in chat
        ↓                 (e.g. "actually the worker uses phone+OTP not phone+password")
        ↓
   [Stage 3+4+5] GENERATE
        ↓
        chainapi lint
        ↓
        if errors → [Stage 6] FIX
        ↓
        chainapi run <op> --dry-run    ← validate without hitting the live API
        ↓
        chainapi run <op>              ← real run
        ↓
        if backend rejects → schema vs reality drift → iterate
```

## Lessons from the GiGwala validation run

Real bugs the single-prompt schema produced:

| Bug | Fix |
|---|---|
| Hardcoded phone `+919876543210` (uniqueness collision on every re-run) | Stage 5 should produce env vars for all unique-per-run identifiers |
| `expect_status: 200` for endpoints returning 201 | Stage 1 must enumerate response status codes from the input docs |
| `register_employee` had no `send_otp_for_signup` dependency despite the backend requiring it | Stage 2 must explicitly enumerate prerequisites for every op based on described semantics |
| `admin_organization.verify` body had `notes:` field that the backend rejected | Stage 4 must mark every body field with `(required) / (optional) / (unknown)` and prefer omitting unknown fields |
| `{{employer.org_id}}` referenced an actor session var the actor never produces | Stage 2 must list every variable's producer (which auth step or extraction). Stage 4 verifies references against that list |

The prompts in this directory encode each lesson as an explicit rule the model checks before emitting output.

## Costs and timing

For a typical 50-endpoint API:

| Provider | Model | Cost | Time |
|---|---|---|---|
| Anthropic | claude-sonnet-4.6 | ~$0.25 | ~5 min wall-clock (incl. human review) |
| Anthropic | claude-haiku-4.5 | ~$0.04 | ~3 min wall-clock |
| OpenAI | gpt-5.4 | ~$0.30 | ~5 min wall-clock |
| OpenAI | gpt-5.3 | ~$0.05 | ~3 min wall-clock |

Haiku/mini are good enough for Stage 1+2; switch to Sonnet/4o for Stages 3-5 if accuracy matters.

## Limits

The AI importer is a starting point, not a finisher. Expect to spend 30-60 minutes hand-fixing the generated schema for any API of moderate complexity. The first time you run a generated op against the real backend, plan to fix:

- Body field names (the backend's actual field names rarely match what's in API docs)
- Response shape (which JSONPath actually finds the ID)
- Status codes (especially for create-vs-replace operations)
- Order of `depends_on` edges (the model often gets the order right but misses transitive deps)

This is Phase 3 work — the desktop UI's "Review and Edit" panel will make this fast. For now, do it in your editor with `chainapi lint` running in a watch loop.
