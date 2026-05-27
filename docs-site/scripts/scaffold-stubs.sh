#!/usr/bin/env bash
# scaffold-stubs.sh — generate placeholder pages for every nav slug so
# the docs site builds end-to-end. Run from docs-site/.
#
# Each stub has frontmatter and a short message pointing at the canonical
# source (existing markdown in doc/ or doc/local/) where applicable.
set -euo pipefail

DOCS=src/content/docs

stub() {
    local slug="$1"; shift
    local title="$1"; shift
    local desc="$1"; shift
    local body="${1:-Coming soon. This page is part of the ChainAPI documentation roadmap.}"
    local path="$DOCS/$slug.md"
    mkdir -p "$(dirname "$path")"
    if [[ -f "$path" ]]; then
        echo "skip (exists): $slug"
        return
    fi
    # YAML-safe-quote the description: replace any " inside with \"
    local desc_quoted="${desc//\"/\\\"}"
    cat > "$path" <<EOF
---
title: $title
description: "$desc_quoted"
---

$body
EOF
    echo "wrote:        $slug"
}

# ─── Concepts ────────────────────────────────────────────────────────────────
stub "concepts/actors" "Actors" \
    "Actor abstraction in ChainAPI: identities with their own auth flows, session caches, and injected headers." \
    "Actors are identities with their own auth flows. Each actor defines a sequence of HTTP requests that produce a session, and a set of headers to inject into every operation that runs as this actor.

See [Mental model](/concepts/mental-model/) and [Auth strategies](/schema/auth-strategies/) for the concrete details. Full content for this page is part of Phase 2 documentation."

stub "concepts/resources" "Resources & operations" \
    "Resources group operations by domain entity. Each operation captures HTTP method, path, body, expected status, and what to extract from the response." \
    "Resources are domain entities (\`order\`, \`payment\`, \`refund\`). Each resource holds a set of named operations — individual HTTP endpoints.

See [Schema authoring guide](/schema/authoring/) for examples and the [Schema spec](/reference/schema-spec/) for the full field reference. Full content for this page is part of Phase 2 documentation."

stub "concepts/dependencies" "Dependency resolution" \
    "How ChainAPI builds the prerequisite chain for any target operation: implicit edges from variable references plus explicit depends_on declarations." \
    "When you ask to run an operation, the engine resolves the dependency chain by combining:

1. **Implicit edges** — any \`{{X.y}}\` reference in a path/body/header implies a dep on whichever operation produces \`X.y\`
2. **Explicit edges** — \`depends_on:\` declarations for prerequisites that don't produce values
3. **Topological sort** with deterministic lexicographic tie-break

See [Mental model](/concepts/mental-model/) for the algorithmic overview and [Engine requirement](/reference/engine-requirement/) §3.1 for the full spec."

stub "concepts/variables" "Variables & references" \
    "Six namespaces of variable references in ChainAPI: builtins, actor sessions, resource extractions, environment, secrets." \
    "ChainAPI's variable substitution syntax is \`{{<scope>.<field>}}\`. Resolution order:

1. Builtins (\`{{\$.uuid}}\`, \`{{\$.now}}\`, \`{{\$.faker.*}}\`)
2. Actor sessions (\`{{<actor>.<var>}}\`)
3. Resource extractions (\`{{<resource>.<var>}}\`)
4. Environment variables (\`{{env.<key>}}\`)
5. Secrets (\`{{secret.<key>}}\`)

See the [Variable syntax reference](/reference/variables/) for the full grammar."

stub "concepts/sessions" "Sessions & caching" \
    "Per-actor session caching, TTL, automatic refresh, and the rules around when sessions are reused vs re-authenticated." \
    "Each actor's auth flow produces a session: a token plus extracted variables, with a configurable TTL. Sessions are cached across operations within a run; the engine automatically reuses them if live, refreshes them if expired (and \`session.refresh\` is configured), or re-authenticates on \`401\`.

See [Engine requirement](/reference/engine-requirement/) §3.3 for the full session lifecycle."

# ─── Schema ──────────────────────────────────────────────────────────────────
stub "schema/file-structure" "File structure" \
    "Multi-file vs single-file project layouts, glob imports, and the two accepted YAML shapes." \
    "ChainAPI projects are folders. Two structural styles are accepted:

- **Multi-file** (preferred for projects with > 5 resources) — separate files for actors, resources, environments, glued together by \`imports:\` in the project root \`chainapi.yaml\`.
- **Single-file** — everything inline. Good for very small projects or examples.

See the [authoring guide](/schema/authoring/) for examples of both shapes."

stub "schema/auth-strategies" "Auth strategies" \
    "Six auth strategies: simple, chain, api_key, oauth2_client_credentials, oauth2_password, oauth1." \
    "ChainAPI ships six auth strategies covering the patterns 95% of APIs use:

| Strategy | When to use |
|---|---|
| \`simple\` | Single login request returns a token |
| \`chain\` | Multi-step (e.g. send-OTP → verify-OTP) |
| \`api_key\` | Pre-issued static credential |
| \`oauth2_client_credentials\` | Client-credentials grant |
| \`oauth2_password\` | Resource-owner password grant |
| \`oauth1\` | OAuth 1.0a signed requests |

Detailed YAML examples for each are in the [authoring guide](/schema/authoring/)."

stub "schema/pitfalls" "Common pitfalls" \
    "The 10 most common schema bugs and their fixes, distilled from real-world LLM-generated and hand-written schemas." \
    "Every bug in this list happened on a real schema during validation. Patterns to avoid:

1. **Wrong \`expect_status\`** — \`200\` vs \`201\` vs \`204\` mismatches
2. **Cross-actor session-var reference** — \`{{employer.org_id}}\` when employer isn't authenticated
3. **Missing OTP / CSRF prerequisite** — auth chains that need a setup step
4. **Hallucinated body fields** — fields the backend rejects
5. **Hardcoded unique value** — works once, fails on re-run
6. **Wrong JSONPath for extraction** — schema vs response shape mismatch
7. **Mixed \`body:\` and \`body_form:\`** — only one per operation
8. **Trailing slash mismatch** — backends are strict
9. **\`baseUrl\` includes the version** — leads to doubled paths
10. **Unquoted YAML scalars** — \`otp: 123456\` parses as int

The full list with concrete fixes is in the [authoring guide](/schema/authoring/) §Common pitfalls."

stub "schema/cheatsheet" "Cheat sheet" \
    "Quick-reference table of common schema patterns and CLI flags." \
    "| You want to... | Pattern |
|---|---|
| Pass a body field unique per run | \`{{env.X}}\` + \`--var X=value\` |
| Pass a body field unique within the run | \`{{\$.uuid}}\` |
| Reference an ID from a previous response | \`{{<resource>.<extracted_name>}}\` |
| Reference an actor's auth token | \`{{<actor>.<token_var>}}\` (in \`inject.headers\`) |
| Force a step to re-run | \`force: true\` on the op |
| Make an op public | Omit the \`actor:\` field entirely |
| Send form-encoded body | Use \`body_form:\` instead of \`body:\` |
| Run against a different env | \`--env staging\` |
| Preview the chain without sending | \`--dry-run\` |
| Override an env var per run | \`--var key=value\` |"

# ─── CLI ─────────────────────────────────────────────────────────────────────
stub "cli/overview" "CLI overview" \
    "The chainapi command-line interface: run, lint, import, dry-run, environments, and CI-friendly output formats." \
    "The CLI is the daily-driver tool for ChainAPI. Three subcommands:

- [\`chainapi run\`](/cli/run/) — execute an operation chain
- [\`chainapi lint\`](/cli/lint/) — validate the schema
- [\`chainapi import\`](/cli/import/) — convert OpenAPI / Postman / Bruno / curl logs

Common flags that work across commands:

- \`--project <path>\` — path to the project root (defaults to cwd)
- \`--env <name>\` — select environment file (defaults to \`local\`)
- \`--var key=value\` — override env vars at run time"

stub "cli/run" "chainapi run" \
    "Execute an operation chain. Auto-resolves prerequisites, runs in topological order, prints HTTP status and timing per step." \
    "\`\`\`
chainapi run <resource.operation> [flags]
\`\`\`

Flags:

| Flag | Purpose |
|---|---|
| \`--project <path>\` | Project root (default: cwd) |
| \`--env <name>\` | Environment file to load (default: \`local\`) |
| \`--var key=value\` | Override an env variable for this run |

The CLI prints a chain summary on completion. On failure, it shows the
HTTP status received and the first 200 chars of the response body for
the failing step.

Full content for this page is part of Phase 2 documentation."

stub "cli/lint" "chainapi lint" \
    "Validate the schema and dependency graph without making any HTTP requests." \
    "\`\`\`
chainapi lint [--project <path>]
\`\`\`

Validates:

- YAML syntax of every project file
- Schema version compatibility
- All \`{{X.y}}\` references trace to a real producer
- Dependency graph has no cycles
- Every operation's chain resolves cleanly

Exits 0 on success, non-zero on any error. Use in pre-commit hooks and
CI pipelines."

stub "cli/import" "chainapi import" \
    "Convert OpenAPI specs, Postman collections, Bruno files, Insomnia exports, or curl logs into a ChainAPI project." \
    "\`\`\`
chainapi import <file>
\`\`\`

The direct importer (no LLM) supports:

- OpenAPI 3.x (YAML or JSON)
- Postman Collection v2.1+
- Bruno collections (folder of \`.bru\` files)
- Insomnia v4 exports

For Markdown docs and curl logs, use the [AI importer](/ai-importer/playbook/)
which leverages an LLM with multi-stage prompts.

Full content for this page is part of Phase 3 of the roadmap."

# ─── AI Importer ─────────────────────────────────────────────────────────────
stub "ai-importer/prompts" "Multi-stage prompt suite" \
    "Six prompts that turn API documentation into a runnable ChainAPI project. Read the playbook first." \
    "The prompt suite splits AI schema generation into six stages, each with its own review gate. See the [AI importer playbook](/ai-importer/playbook/) for the full workflow.

The prompts themselves live at \`prompts/import/\` in the repository:

| Stage | Prompt file | Purpose |
|---|---|---|
| 1 | \`01-discover.md\` | Structured digest of the API |
| 2 | \`02-plan-schema.md\` | Written schema plan (human-reviewable) |
| 3 | \`03-generate-actors.md\` | YAML files for actors |
| 4 | \`04-generate-resources.md\` | YAML files for resources (one per call) |
| 5 | \`05-generate-environment.md\` | Environment file with placeholders |
| 6 | \`06-fix-lint-errors.md\` | Iterative fix-up |"

stub "ai-importer/openapi" "Importing from OpenAPI" \
    "Use the AI importer with OpenAPI 3.x specs. Direct parser available for non-LLM imports." \
    "Two paths for OpenAPI input:

1. **Direct parser (no LLM)** — \`chainapi import openapi.yaml\`. Faster, deterministic, free.
2. **AI importer with prompts** — better for OpenAPI specs that lack rich descriptions or have unusual auth flows.

Use the direct parser first; fall back to the AI importer if the result needs significant editing.

See [AI importer playbook](/ai-importer/playbook/) for the prompt workflow."

stub "ai-importer/postman" "Importing from Postman" \
    "Use ChainAPI's direct Postman importer. Faster and more reliable than going through an LLM." \
    "Postman collections (v2.1+) import directly without an LLM:

\`\`\`bash
chainapi import postman-collection.json
\`\`\`

The importer:

- Maps Postman folders to resources (best-effort, with manual override)
- Maps Postman environments to ChainAPI environments
- Converts pre-request and test scripts to ChainAPI hooks (with warnings for unsupported APIs)
- Detects token-extraction patterns and converts them to declarative \`extract:\` blocks

Plan to spend 15-30 min reviewing the output for any non-trivial collection."

stub "ai-importer/curl" "Importing from curl logs" \
    "Capture a session of curl commands and have the AI importer infer the actor + resource structure." \
    "Paste your curl logs into the [AI importer prompt suite](/ai-importer/prompts/) starting from Stage 1.

This is the hardest input format — no formal schema, no metadata. Expect Stage 1 to surface many open questions. Spending 10 minutes answering them in chat saves hours of rework downstream.

See [AI importer playbook](/ai-importer/playbook/) for the workflow."

# ─── Examples ────────────────────────────────────────────────────────────────
stub "examples/marketplace" "Marketplace API example" \
    "30-endpoint two-sided marketplace sample with admin/vendor/customer actors. The canonical reference project." \
    "The bundled \`samples/marketplace/\` project models a two-sided marketplace:

- 3 actors: admin (email + password), vendor (email + password + refresh), customer (phone + OTP chain)
- 5 resources: products, cart, orders, refunds, reviews
- 27 operations covering the full e-commerce flow

Try it:

\`\`\`bash
chainapi lint --project samples/marketplace
chainapi run refund.approve --project samples/marketplace
\`\`\`

The repository structure: see \`samples/marketplace/\` in the source tree.

Full annotated walkthrough is part of Phase 2 documentation."

stub "examples/github" "GitHub REST API example" \
    "22-endpoint GitHub validation project: PAT auth, header-based pagination, deep dependency chains." \
    "The validation project at \`validation/github/\` (in the source tree) models a 22-endpoint slice of GitHub's REST API. Highlights:

- Two actors (user + admin PAT) with the same auth strategy but different scopes
- Header-based pagination via \`Link: <...>; rel=\"next\"\`
- Deep dependency chain: repo → branch → content → pull → merge

This was one of the three Phase 0 validation APIs; the findings document at \`validation/github/findings.md\` (local-only) details the schema gaps that informed the spec.

Full annotated walkthrough is part of Phase 2 documentation."

stub "examples/stripe" "Stripe API example" \
    "24-endpoint Stripe validation project: form-encoded bodies, idempotency keys, multi-tenant headers (Stripe-Account)." \
    "The validation project at \`validation/stripe/\` models a 24-endpoint slice of Stripe's API. Highlights:

- Form-encoded bodies (\`application/x-www-form-urlencoded\`)
- Mandatory \`Idempotency-Key\` headers via \`{{\$.uuid}}\`
- Multi-tenant via \`Stripe-Account\` header — same credential, different acting identity per request (modeled as two actors with different \`inject.headers\`)
- 5 resources spanning customer → payment_method → payment_intent → refund

Full annotated walkthrough is part of Phase 2 documentation."

# ─── Reference ───────────────────────────────────────────────────────────────
stub "reference/schema-spec" "Schema specification" \
    "The canonical YAML schema spec — every field, every type, every default." \
    "The full schema specification is in [\`doc/ChainAPI - PRD.md\`](https://github.com/chainapi/chainapi/blob/main/doc/ChainAPI%20-%20PRD.md) §5 in the source tree.

Key reference tables:

- §5.5: Actor definitions (auth strategies, sessions, inject headers)
- §5.6: Resource and operation definitions
- §5.7: Variable resolution rules
- §5.8: Dependency resolution algorithm

A polished web-friendly version of this spec is part of Phase 2 documentation."

stub "reference/variables" "Variable syntax reference" \
    "Every namespace, every builtin, the full grammar for {{X.y}} references." \
    "Variable references use the syntax \`{{<scope>.<field>}}\`. Six scopes:

| Scope | Source | Example |
|---|---|---|
| \`\$\` | Builtins | \`{{\$.uuid}}\`, \`{{\$.now}}\`, \`{{\$.faker.email}}\`, \`{{\$.env.HOME}}\` |
| \`<actor>\` | Actor session vars | \`{{vendor.token}}\` |
| \`<resource>\` | Resource extractions (most-recent) | \`{{order.order_id}}\` |
| \`<resource>[N]\` | Resource extractions (indexed) | \`{{order[2].order_id}}\` |
| \`env\` | Environment variables | \`{{env.baseUrl}}\` |
| \`secret\` | OS keychain | \`{{secret.STRIPE_KEY}}\` |

Resolution order: builtins → actor → resource → env → secret.

Full content for this page is part of Phase 2 documentation."

stub "reference/error-codes" "Error codes" \
    "Every E_* code the engine emits, what triggers it, and whether it's retryable." \
    "The engine produces stable error codes for every failure mode. UI / CLI / tests assert on these.

| Code | Class | Retryable | Common cause |
|---|---|---|---|
| \`E_SCHEMA_INVALID\` | Schema | No | Malformed YAML or schema version mismatch |
| \`E_CYCLE\` | Schema | No | Circular dependency in chain |
| \`E_REF_UNDEFINED\` | Schema | No | \`{{X.y}}\` refers to non-existent producer |
| \`E_VAR_UNRESOLVED\` | Resolution | No | Required variable couldn't be substituted |
| \`E_NETWORK_TIMEOUT\` | Network | Yes | Read or connect timeout |
| \`E_NETWORK_DNS\` | Network | Yes | DNS resolution failed |
| \`E_NETWORK_TLS\` | Network | No | TLS handshake failure |
| \`E_HTTP_5XX\` | HTTP | Yes | Server-side 5xx |
| \`E_HTTP_4XX\` | HTTP | No | Client-side 4xx |
| \`E_STATUS_MISMATCH\` | HTTP | No | Status code didn't match \`expect_status\` |
| \`E_SESSION_REFRESH_FAILED\` | Auth | No | Auth flow returned an error |
| \`E_HOOK_FAILURE\` | Hook | No | JS pre/post hook threw or timed out |
| \`E_EXTRACTION_FAILED\` | Extraction | No | JSONPath didn't match the response |
| \`E_RESPONSE_PARSE\` | Extraction | No | Response declared JSON but didn't parse |
| \`E_CANCELLED\` | Run | No | User cancelled |

Full taxonomy is in [Engine requirement](/reference/engine-requirement/) §5."

stub "reference/engine-requirement" "Engine requirement (full spec)" \
    "The complete engine specification: state machines, error taxonomy, edge cases, and acceptance criteria." \
    "The full engine specification is at [\`doc/ChainAPI - Engine Requirement.md\`](https://github.com/chainapi/chainapi/blob/main/doc/ChainAPI%20-%20Engine%20Requirement.md) in the source tree.

Sections:

- §3 Functional acceptance criteria (40+ AC items)
- §4 State machines (operation step, session, resource extraction, run, schema load)
- §5 Error taxonomy
- §6 Concurrency and cancellation
- §7 Edge cases
- §8 Test scenarios (QA-ready)

This page will eventually inline the spec for offline reading. For now the source markdown is canonical."

# ─── Development ─────────────────────────────────────────────────────────────
stub "dev/architecture" "Architecture" \
    "The two-phase architecture: in-process engine for MVP, extractable to a separate process or Rust later." \
    "ChainAPI's architecture is documented in [\`doc/ChainAPI - Project Layout.md\`](https://github.com/chainapi/chainapi/blob/main/doc/ChainAPI%20-%20Project%20Layout.md) and PRD §8.

Key principles:

- **Engine boundary** — \`libchainapi-engine\` has no Qt UI dependency. Mechanically enforced via CMake link guards, CI grep checks, and pImpl public headers.
- **Layered C++** — domain → application → infrastructure, dependencies pointing inward only.
- **Phase B option** — the engine is constructable as an in-process library today and extractable to a separate process or rewritten in Rust later. Architectural guardrails make this a build-system change, not a rewrite.

Full content for this page is part of Phase 2 documentation."

stub "dev/building" "Building from source" \
    "Full build instructions for macOS, Linux, Windows. Prerequisites, troubleshooting, presets." \
    "See the [installation guide](/start/install/) for end-to-end build instructions. Quick reference:

| Platform | Preset | Notes |
|---|---|---|
| macOS | \`macos-debug\` / \`macos-release\` | Uses Homebrew Qt; vcpkg for everything else |
| Linux | \`linux-debug\` / \`linux-release\` | Full vcpkg-based; Qt built from source on first run |
| Windows | \`windows-debug\` / \`windows-release\` | Visual Studio 2022 + Qt online installer |

CI uses Linux + Windows presets with full vcpkg builds. Local macOS uses Homebrew Qt for speed.

Full content for this page is part of Phase 2 documentation."

stub "dev/contributing" "Contributing" \
    "How to contribute: development workflow, code style, the architectural firewall, the boundary check, PR guidelines." \
    "Contributing guidelines:

- Apache 2.0 license; sign-off required (DCO)
- Conventional commits (\`feat:\`, \`fix:\`, \`docs:\`, etc.)
- Run \`./tools/pre-push-check.sh\` before pushing
- Engine changes must keep the architectural firewall intact (no Qt UI deps)
- Tests required for all new features and bug fixes (80%+ coverage in domain layer)

The full contribution guide is in [\`AGENTS.md\`](https://github.com/chainapi/chainapi/blob/main/AGENTS.md) in the source tree.

Polished contributor docs are part of Phase 2 documentation."

stub "dev/roadmap" "Roadmap" \
    "What's done, what's next: Phase 1 engine + CLI complete; Phase 2 desktop UI in progress; Phase 3+ deferred." \
    "Roadmap as of the most recent release:

| Phase | Status | Highlights |
|---|---|---|
| 0 — Validation | ✅ Complete | Schema validated against GitHub / Stripe / Discourse / GiGwala |
| 1 — Engine + CLI | ✅ Complete | 188 tests green, real-world validation against live backend |
| 2 — Desktop UI | 🚧 In progress | Project explorer, request editor, response viewer, dependency graph |
| 3 — AI importer + Postman migration | Planned | Multi-stage prompt suite + direct format converters |
| 4 — Polish & v1 launch | Planned | Auto-update, sample projects, public release |
| 5+ — Mock server, team sync, hosted | Post-MVP | Per PRD §13.6 |

The canonical roadmap is in [\`doc/ChainAPI - PRD.md\`](https://github.com/chainapi/chainapi/blob/main/doc/ChainAPI%20-%20PRD.md) §13."

echo ""
echo "Stub generation complete."
