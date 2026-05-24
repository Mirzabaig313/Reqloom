# Phase 0 Validation

> Per PRD §13.1 and §16, the schema spec must be validated against three real-world APIs before any MVP code is written. This directory holds the validation work and the kits for the user-driven parts.

## Layout

```
validation/
├── README.md                       # This file
├── github/                         # Validation against GitHub REST API
│   ├── chainapi.yaml
│   └── findings.md
├── stripe/                         # Validation against Stripe API
│   ├── chainapi.yaml
│   └── findings.md
├── discourse/                      # Validation against Discourse API
│   ├── chainapi.yaml
│   └── findings.md
├── llm-feasibility/                # User-executed: LLM importer feasibility test
│   ├── README.md
│   ├── system-prompt.md
│   ├── inputs/
│   ├── rubric.md
│   └── results-template.md
└── design-partners/                # User-executed: design-partner interviews
    ├── outreach-email.md
    ├── screening-criteria.md
    ├── interview-script.md
    └── results-template.md
```

## Why these three APIs

Each was chosen to stress a specific corner of the schema spec.

| API | What it stress-tests |
|---|---|
| **GitHub** | Multiple auth strategies (PAT vs App installation), header-based pagination cursors, deep dependency chains, role hierarchies |
| **Stripe** | `application/x-www-form-urlencoded` bodies (not JSON), idempotency-key generation, multi-tenant headers (`Stripe-Account`), expansions |
| **Discourse** | Two-header auth (`Api-Key` + `Api-Username`), single-key-multi-actor pattern, role-permission stratification, CRUD with realistic dependencies |

If the schema expresses all three without escape hatches, PRD §16.2 gate question 1 ("can express all 3 sample APIs without escape hatches") passes.

## Synthesized result

See [`doc/Phase 0 - Validation Report.md`](../doc/Phase%200%20-%20Validation%20Report.md) for the cross-API findings and the go/no-go decision.
