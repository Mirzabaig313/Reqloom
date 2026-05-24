# LLM Output Scoring Rubric

> Score each generated YAML against this rubric. One score per (input, provider) pair.

## Quick scoring rules

- **2 points** — Correct without edits
- **1 point** — Mostly correct; minor edits needed (renamed a field, missed an `expect_status:`)
- **0 points** — Wrong or missing

For each input, score each criterion below 0/1/2, then divide by the maximum and convert to a percentage.

---

## Criteria — common to all inputs

| # | Criterion | Notes |
|---|---|---|
| C1 | Output is valid YAML | If it parses with any YAML parser, score 2. If it has minor indentation issues that a human can fix in <30s, score 1. |
| C2 | `version: 1` declared at the top | |
| C3 | `name:` field set to a sensible value | |
| C4 | Operations grouped into resources by noun | e.g. all `/users/...` paths under `user:`. Score 2 if grouping is correct; 1 if 1–2 misplacements; 0 if grouping is incoherent. |
| C5 | No invented endpoints | Score 2 if every operation maps to something in the input; 0 if the model hallucinated |

## Criteria — actor inference

| # | Criterion |
|---|---|
| A1 | Number of actors = number of distinct security schemes / roles in the input |
| A2 | Each actor has a sensible `auth:` block (verification call OR login chain matching the input) |
| A3 | Actor `inject:` references the right credential field |
| A4 | Session TTL is reasonable (15m–24h depending on auth pattern) |

## Criteria — operation inference

| # | Criterion |
|---|---|
| O1 | Every documented endpoint becomes an operation |
| O2 | HTTP method is correct |
| O3 | Path is correct (with `{{ref.field}}` substitution where appropriate) |
| O4 | Body fields match the input |
| O5 | `actor:` is set correctly per operation |

## Criteria — dependency inference

| # | Criterion |
|---|---|
| D1 | `depends_on:` is present where one operation needs the output of another |
| D2 | Variable references like `{{order.order_id}}` resolve to a producing operation |
| D3 | `extract:` is set on operations that produce values used downstream |

---

## Aggregate score

For each (input, provider) pair:

```
score = sum(criterion_scores) / (number_of_criteria × 2) × 100
```

Number of criteria: 5 common + 4 actor + 5 operation + 3 dependency = **17**, max raw score 34.

## Pass thresholds

Per PRD §16.2 + FR-9.5:

| Input | Threshold to pass |
|---|---|
| `01-petstore-openapi.yaml` (OpenAPI) | **≥ 80%** |
| `02-github-rest-snippet.md` (Markdown) | **≥ 70%** |
| `03-curl-list.txt` (curl) | **≥ 60%** |

A provider **passes overall** if it hits all three thresholds. The §16.2 gate clears if **at least one** provider passes overall.

## Common failure modes to watch for

- **Missing actors when only one security scheme exists** — model produces no actors block, or wraps all ops in a single `default` actor without explaining why
- **Missing dependency edges** — model produces correct operations but forgets `depends_on:`, breaking the value prop
- **Hallucinated endpoints** — most commonly `health`, `version`, `/metrics`, or "user can also..."
- **Indentation cascading** — one indent error early breaks the rest of the YAML
- **Mixing JSON and YAML** — model wraps body values in `{...}` with quoted keys
- **Inventing `expect_status` values** — using `200` everywhere, including for endpoints documented as returning `201` or `204`
