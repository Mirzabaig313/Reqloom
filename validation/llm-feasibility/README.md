# LLM Importer Feasibility Test

> **Goal:** answer PRD §16.2 question 3 — "Is LLM accuracy ≥80% on OpenAPI input?" — without writing any code.
> **Time required:** ~3 hours of human time across one or two model providers.
> **Cost:** typically under $5 in API credits across the three test inputs.

This kit lets you run the test in a single sitting using just the LLM provider's web UI or playground. No code, no orchestration.

## Procedure

1. Pick one or two LLM providers from the **Providers** list below.
2. For each provider:
   - Open the playground/chat in a fresh session
   - Paste **`system-prompt.md`** as the system prompt
   - For each of the three inputs in `inputs/`, paste it as a single user message and capture the response
3. Score each response against `rubric.md`
4. Fill in `results-template.md`
5. Compute the aggregate score; compare against the §16.2 threshold

## Providers

The PRD §10.4 commits to OpenAI, Anthropic, and Ollama support. Run the test against at least two:

- **OpenAI** — try `gpt-4o-mini` first (cheap), then `gpt-4o` if the cheap model fails
- **Anthropic** — try `claude-haiku-4.5` first, then `claude-sonnet-4.5` if needed
- **Ollama (local)** — optional; run if you can spare the time. Test `llama3.1:70b` or similar

## Inputs

Three test inputs of increasing difficulty:

| Input | Source | Why this input |
|---|---|---|
| `inputs/01-petstore-openapi.yaml` | The canonical OpenAPI 3 sample | Easiest case; if the model fails this, the whole approach is dead |
| `inputs/02-github-rest-snippet.md` | Markdown snippet of GitHub REST docs | Tests Markdown parsing — the second-most-common importer input |
| `inputs/03-curl-list.txt` | Pasted curl commands for the marketplace flow | Tests the hardest case — free-form input with no formal spec |

## Pass/fail thresholds (per PRD FR-9.5 + §16.2)

A run **passes** if, on the OpenAPI input, all four of these hold:

- ≥ 90% of paths in the spec become operations (count: operations correctly inferred / paths in spec)
- ≥ 90% of `securitySchemes` become actors
- ≥ 80% of `requestBody` fields appear in the generated `body:` blocks
- The output is valid YAML that a human can read without translation

A run **passes the gate** if both providers tested pass on the OpenAPI input AND at least one passes on the Markdown input.

## What to do with the results

If the test passes:
- Document the prompt + chosen provider as the v1 default in `prompts/import/system.md`
- Note any input format that needs special pre-processing (e.g. "Markdown sources benefit from explicit hints about which sections describe auth")
- Greenlight Phase 3 of the roadmap (the AI importer)

If the test fails:
- Compare what each provider got wrong — pattern in the errors, or random?
- Iterate on `system-prompt.md` (add few-shot examples; tighten output format spec)
- If after 3 iterations the score is still <70%, consider scoping the AI importer down: ship "OpenAPI direct parser" only in v1 (no LLM), defer Markdown/curl inference to Phase 4
