---
title: Roadmap
description: "What works today, what is partial, and what is deliberately deferred — measured against the requirement set rather than estimated."
---

Reqloom is pre-1.0. This page says what actually works, because "in progress" on
its own isn't useful when you're deciding whether to adopt something.

The numbers come from an internal audit that checks every requirement against
source with file-level evidence, rather than against intent.

## Where it stands

**1,129 automated tests.** Against the 92 functional requirements Reqloom is specified against:

| Status | Count |
| --- | --- |
| Done | 60 |
| Partial | 10 |
| Not done | 20 |
| Deferred (post-MVP by design) | 2 |

Weighted completion, counting partial as half: **71%**.

## By area

| Area | Completion | Notes |
| --- | --- | --- |
| Response viewer | 100% | |
| Request editor | 100% | |
| Dependency graph | 100% | Shipped ahead of its phase |
| CLI | 92% | |
| Execution engine | 91% | The core is essentially complete |
| HTTP client | 89% | Only WebSocket/SSE missing, deliberately deferred |

The engine and CLI are the mature parts. That's deliberate — they're what the
desktop app and CI both depend on.

## What works today

Everything documented on this site is implemented. The load-bearing pieces:

- **Dependency resolution** — implicit edges from references, explicit
  `depends_on`, Kahn's sort with a deterministic tie-break, cycle and reference
  checks at load time
- **Eleven auth strategies** plus per-operation inline auth, session caching,
  token refresh, and a per-actor cookie jar
- **Polling**, `for_each` fan-out, response assertions with grouping and regex,
  sandboxed JS pre/post hooks, file uploads
- **Retries** with backoff that never repeat a POST or PATCH that may have
  reached the server, unless the operation opts in
- **Three output formats** — text, JSON, JUnit — for CI
- **Direct importers** for nine formats (Postman, Bruno, Insomnia, Hoppscotch,
  Apidog, Thunder Client, HTTPie, REST Client, OpenAPI/Swagger), and the AI
  importer for unstructured input
- **Secrets** in the OS keychain, stored per project and masked by value in
  everything the engine emits
- **Desktop app** — project explorer, request editor, response viewer with diff,
  run timeline, run history, dependency graph

## Known gaps

Honest list of what you'll notice missing:

| Gap | Impact |
| --- | --- |
| WebSocket / SSE | No streaming protocol support. Deferred by design |
| No `reqloom migrate` | The error message for an unsupported `version:` suggests it; it doesn't exist yet |
| No CLI secret management | Secrets go in via the desktop app or your OS keychain tool, and only the desktop app can confirm a moved project |
| No XPath extraction | `source: xpath` parses but never yields a value |
| No regex time limit | Patterns are screened for nested repeats, but a slow pattern the check misses can still be slow |
| Imported credentials | Only flagged variables and literal `Authorization`, `Proxy-Authorization` and `X-API-Key` headers move into secrets; a token in a body or query string stays in the YAML |
| Captured bodies in clear | With Capture bodies on, run history stores response bodies as received, tokens included |
| Cookie scope after redirects | A cookie set after a redirect is stored under the original request's host; a cookie without `Path` is sent on every path |
| No `--version` flag | Check the release you downloaded |
| Unknown keys only warn | A misspelled key is a lint warning, not an error, and `reqloom run` doesn't show it — see [pitfalls](/schema/pitfalls/) |
| Unsigned builds | macOS and Windows warn on first launch. Needs a paid certificate |
| Desktop UI still maturing | Functional, less polished than the CLI |

## Coming next

The direction is **Human + AI**: people and AI coding agents work on the same
project through the same engine. The desktop app stays; an agent surface is added
beside it. The guiding rule is unchanged — the **same `reqloom.yaml`** powers
every feature, with no parallel definitions.

**Nothing in this section is built yet.** Stages are an order, not a schedule.

### Stage 1 — The agent door and the live view

- **Graph commands in the CLI** — `list`, `describe`, and `plan` (what must run
  before an operation, as which actor) with stable JSON output, so an agent can
  ask the dependency graph instead of guessing call order and IDs
- **Responses in CLI output** — every step's status code, body, and extracted
  values, with secrets masked, so a run can be inspected from the terminal
  (today only a failure message quotes the response)
- **Headless secrets** — `REQLOOM_SECRET_<NAME>` for CI and cloud sandboxes that
  have no OS keychain
- **Agent integration** — a skill file, an `AGENTS.md` snippet, a thin MCP
  server, and plugins for Claude Code, Codex, Gemini CLI, and Cursor
- **Live session** — a chain an agent runs from the terminal appears in the
  desktop app's timeline as it executes, labelled with who ran it
- **Hand-off** — send a failed step from the app to the agent
- **Host approval** — credentials are sent to a host only after you have
  approved that host for the project; the project file alone never grants it

### Stage 2 — Proof that a change works

- **Permission matrix** — run an operation as every actor and check who is
  allowed, forbidden, or sees nothing, including another tenant's records
- **`reqloom verify`** — one command for lint, assertions, and the matrix, with
  `--affected` to run only the chains touching changed endpoints, plus a
  ready-made agent stop hook, pre-push hook, and GitHub Action
- **Evidence bundle** — a pull-request summary of what ran, as whom, and what
  came back
- **Behaviour diff** — compare a run with the last accepted baseline

### Stage 3 — QA and scale

- **Test data lifecycle** — per-run unique data and declared cleanup that runs
  even when a run fails, so parallel runs do not collide
- **Review queue** — agent runs listed in the app with their evidence and diffs
- **Negative cases** — wrong actor, missing credentials, and someone else's ID,
  generated with prerequisites already satisfied
- **OpenAPI conformance** — check live responses against the spec inside chains

### Stage 4 — Agent panel in the app

- A chat panel that hosts your own coding agent through the Agent Client
  Protocol. Reqloom ships no model and no agent of its own.

### Finishing the core

Gaps against the current product spec:

- **Binary request bodies** — send a file as the body itself, not only as a
  multipart part
- **Fuller Postman migration** — import Postman environment files and convert
  pre-request and test scripts to hooks
- **History tools** — filter runs, compare two runs of the same operation, and a
  retention limit
- **Command palette over operations** — jump to and run any operation by name
- **Live reload** — pick up project files changed on disk
- **Single-file export** — bundle a project into one YAML file for sharing
- **Releases** — auto-update, notarized macOS and signed Windows builds, and
  `.deb` / `.rpm` packages

### Also planned

- **AI doc import with review** — generate a project from API docs with a
  verification pass and a review screen. Today this is a prompt you give to your
  own model, checked with `reqloom lint` and `reqloom run` — see the
  [AI importer](/ai-importer/playbook/)
- **Mock server mode** — serve recorded or templated responses for a project's
  operations from a local HTTP server
- **Schema-driven documentation** — render a project to a browsable docs site
  with request and response examples captured from real runs
- **Conditional dependencies and scenarios** — `when:`-guarded prerequisites and
  named multi-step regression scenarios, still declared in YAML
- **Browser request capture** — build a project from requests captured in a
  running app
- **Team sync** — git-based sharing of projects and run state
- **Hosted version** — team workspaces, and a browser-based client for staging
  and production APIs
- **Streaming protocols** — WebSocket, SSE, and GraphQL subscription support

## Stability commitments

Two things you can build on before 1.0:

- **The schema is versioned.** `version:` is required and the loader accepts 1–3.
  A future breaking change gets a new version number, not a silent
  reinterpretation.
- **Error codes are stable.** `E_*` strings are a contract shared by the CLI,
  desktop, and JSON/JUnit output, and safe to assert on in CI.

Flags may gain options. Nothing documented here is planned for removal.

## Following along

- [Releases](https://github.com/Mirzabaig313/Reqloom/releases) — builds and changelogs
- [Issues](https://github.com/Mirzabaig313/Reqloom/issues) — bugs and requests
- [Contributing](/dev/contributing/) — if you'd like to close one of the gaps
