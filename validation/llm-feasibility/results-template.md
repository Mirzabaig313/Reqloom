# LLM Feasibility Test — Results

**Date run:** _<YYYY-MM-DD>_
**Run by:** _<name>_
**Time spent:** _<hours>_

## Providers tested

- [ ] OpenAI `gpt-4o-mini`
- [ ] OpenAI `gpt-4o`
- [ ] Anthropic `claude-haiku-4.5`
- [ ] Anthropic `claude-sonnet-4.5`
- [ ] Ollama `<model>` (optional)

## Per-input results

### Input 1 — Petstore OpenAPI

| Provider | C1 | C2 | C3 | C4 | C5 | A1 | A2 | A3 | A4 | O1 | O2 | O3 | O4 | O5 | D1 | D2 | D3 | Total | % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| _provider_ | | | | | | | | | | | | | | | | | | / 34 | % |

Notable issues observed:
- _list any pattern in the failures_

### Input 2 — GitHub Markdown

| Provider | C1 | C2 | C3 | C4 | C5 | A1 | A2 | A3 | A4 | O1 | O2 | O3 | O4 | O5 | D1 | D2 | D3 | Total | % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| _provider_ | | | | | | | | | | | | | | | | | | / 34 | % |

Notable issues observed:
- _list any pattern in the failures_

### Input 3 — curl list

| Provider | C1 | C2 | C3 | C4 | C5 | A1 | A2 | A3 | A4 | O1 | O2 | O3 | O4 | O5 | D1 | D2 | D3 | Total | % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| _provider_ | | | | | | | | | | | | | | | | | | / 34 | % |

Notable issues observed:
- _list any pattern in the failures_

## Aggregate

| Provider | OpenAPI % | Markdown % | curl % | Passes 80/70/60 thresholds? |
|---|---|---|---|---|
| _provider_ | | | | yes / no |

## Decision

**Per PRD §16.2 question 3:**

- [ ] **Pass** — at least one provider hits all three thresholds. Greenlight Phase 3 of the roadmap (AI importer).
- [ ] **Conditional pass** — at least one provider hits OpenAPI threshold but fails Markdown/curl. Ship "OpenAPI direct parser" only in v1; defer Markdown/curl to Phase 4.
- [ ] **Fail** — no provider hits even the OpenAPI threshold. Iterate on the prompt OR scope the AI importer out of MVP entirely.

## Cost summary

| Provider | Inputs run | Approximate cost |
|---|---|---|
| | | $ |

Total: $ _<sum>_

## Saved outputs

The full YAML output from each (provider, input) run is saved under:
```
validation/llm-feasibility/runs/<YYYY-MM-DD>/<provider>/input-<n>.yaml
```

(Create these folders as you go. Saving the raw outputs lets us re-score later if the rubric changes.)

## Recommendations for the v1 prompt

Based on what worked and what didn't, the production system prompt at `prompts/import/system.md` should:

- _list concrete prompt improvements_
- _list any few-shot examples to add_
- _list any input formats that need pre-processing_
