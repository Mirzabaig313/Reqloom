---
title: Roadmap
description: "What works today, what is partial, and what is deliberately deferred — measured against the requirement set rather than estimated."
---

Reqloom is pre-1.0. This page says what actually works, because "in progress" on
its own isn't useful when you're deciding whether to adopt something.

The numbers come from an internal audit that checks every requirement against
source with file-level evidence, rather than against intent.

## Where it stands

**852 tests passing.** Against the 92 functional requirements Reqloom is specified against:

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
  `depends_on`, Kahn's sort with a deterministic tie-break, cycle detection at
  load time
- **Eleven auth strategies** plus per-operation inline auth, session caching, and
  token refresh
- **Polling**, `for_each` fan-out, response assertions, JS pre/post hooks, file
  uploads
- **Three output formats** — text, JSON, JUnit — for CI
- **Direct importers** for seven formats, and the AI importer for unstructured
  input
- **Secrets** in the OS keychain, never on disk
- **Desktop app** — project explorer, request editor, response viewer,
  dependency graph

## Known gaps

Honest list of what you'll notice missing:

| Gap | Impact |
| --- | --- |
| WebSocket / SSE | No streaming protocol support. Deferred by design |
| No `reqloom migrate` | The error message for an unsupported `version:` suggests it; it doesn't exist yet |
| No CLI secret management | Secrets go in via the desktop app or your OS keychain tool |
| No `--version` flag | Check the release you downloaded |
| No strict schema mode | Unknown keys are silently ignored — see [pitfalls](/schema/pitfalls/) |
| Unsigned builds | macOS and Windows warn on first launch. Needs a paid certificate |
| Desktop UI still maturing | Functional, less polished than the CLI |

## Toward 1.0

Roughly in priority order, not on a date:

1. **Signing and notarization** so downloads stop looking broken
2. **Package managers** — Homebrew and Scoop
3. **Closing the partial requirements** — the 10 above
4. **Desktop polish** and auto-update
5. **Strict schema mode**, so misspelled keys become errors instead of silence

## Post-1.0

Listed because people ask, not because they're scheduled:

- Mock server generated from a schema
- Team workspace sync (a paid component — the engine, CLI, schema, and desktop
  stay Apache 2.0)
- Hosted runs

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
