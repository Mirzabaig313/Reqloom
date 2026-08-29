---
title: Architecture
description: "Three deliverables, one engine. Clean-architecture layering, the enforced Qt-UI boundary, and how a run flows through the system."
---

Reqloom is one engine with two front ends. The engine holds all behaviour; the
CLI and desktop app are thin adapters over it. That's why a schema that lints in
one behaves identically in the other.

## Three deliverables

```
reqloom/
├── engine/     libreqloom-engine — pure C++ engine. No Qt UI.
├── cli/        reqloom — links engine + Qt6::Core only
├── desktop/    Reqloom — the only place Qt UI is allowed
└── ipc/        Phase B scaffold (currently empty)
```

| Target | Links | Purpose |
| --- | --- | --- |
| `reqloom::engine` | Qt6::Core, yaml-cpp, nlohmann_json, QuickJS, curl | Schema, resolution, execution |
| `reqloom-cli` | engine + Qt6::Core | The `reqloom` binary |
| `reqloom-desktop` | engine + Qt6 Widgets/Quick | The GUI |

## The engine has no UI dependency

This is the architectural rule the project actually enforces: the engine must not
depend, directly or transitively, on Qt UI or the code editor widget. It's
checked in three places, so it can't rot:

1. **A CMake link guard.** `cmake/ReqloomBoundaryGuards.cmake` fails *configure*
   if the engine, CLI, or engine tests link `Qt6::Widgets`, `Qt6::Gui`,
   `Qt6::Quick`, `Qt6::QuickWidgets`, or `QScintilla`.
2. **A CI grep job.** Rejects any `#include <QWidget>`, `<QApplication>`, or
   `<Qsci…>` under `engine/` or `cli/`.
3. **The public header surface.** `engine/include/reqloom/engine/*.h` uses pImpl
   and value types; no Qt UI type appears.

Why bother: it keeps the engine testable headlessly, keeps the CLI small enough
to run in CI containers, and means the desktop app can be replaced without
touching execution logic.

## Clean architecture inside the engine

```
engine/src/
├── domain/           pure logic, no I/O
├── application/      use cases, orchestration
└── infrastructure/   I/O adapters — HTTP, SQLite, keychain, YAML, hooks
```

Dependencies point **inward**. Domain knows nothing about YAML or HTTP; it
operates on value types and interfaces defined in its own layer.

| Layer | Contains | May depend on |
| --- | --- | --- |
| `domain` | `VariableResolver`, `DependencyResolver`, `Predicate`, `ErrorCodes` | stdlib + Qt6::Core (for `QString`) |
| `application` | Use cases like `RunOperationUseCase`, `ImportFromOpenApi` | domain |
| `infrastructure` | `YamlSchemaParser`, `KeychainSecretStore`, `QuickJsHookRunner`, HTTP client | domain, application |

Each layer is a separate CMake object library, and each is passed through
`reqloom_forbid_dependencies(...)` — so the boundary is a build error, not a
convention.

Infrastructure is reached through interfaces declared in the inner layer. The
secret store is the clearest example: `SecretStore.h` is an abstract interface;
`KeychainSecretStore` implements it behind `REQLOOM_HAS_QTKEYCHAIN`, with a no-op
fallback so the engine still links where QtKeychain is absent.

## How a run flows

```
reqloom run refund.approve
        │
        ▼
  YamlSchemaParser          load root + imports, validate
        │                   → E_YAML_PARSE / E_SCHEMA_VERSION
        ▼
  DependencyResolver        build the graph, Kahn's sort
        │                   → E_REF_UNDEFINED / E_CYCLE
        ▼
  ExecutionEngine           load referenced secrets, then per step:
        │                     ├── authenticate the actor (cached by TTL)
        │                     ├── VariableResolver — substitute {{...}}
        │                     ├── pre_request hook (QuickJS)
        │                     ├── HTTP request
        │                     ├── post_response hook
        │                     ├── poll_until loop, if declared
        │                     ├── extract into the resource
        │                     └── evaluate assertions
        ▼
  RunResult                 steps, statuses, timings, error codes
        │
        ▼
  Text / JSON / JUnit renderer
```

Validation happens once, up front — which is why `reqloom lint` can catch schema
and dependency errors without sending a request.

## Errors are values, not exceptions

Every recoverable failure is `std::expected<T, ReqloomError>`, carrying a stable
`ErrorCode`:

```cpp
std::expected<RunResult, ReqloomError>
RunOperationUseCase::execute(const OperationId& op, RunContext& ctx);
```

There's a single `ErrorCode` enum for the whole engine — no parallel error types
per subsystem. That's what makes the `E_*` strings a contract the CLI, desktop,
and JSON output all share. Exceptions are reserved for genuinely unrecoverable
conditions and never cross an ABI boundary.

## Run state

`RunContext` holds everything mutable for one run: actor sessions, resource
instances, and the step timeline. It's move-only and lives exactly as long as the
run.

Resource **instances** are the reason `{{order.order_id}}` resolves to the newest
order and `{{order[2].order_id}}` to the second — the context keeps a list per
resource, and `for_each` binds an iteration index into it.

## Threading

The engine doesn't create threads. Callers decide.

- Public entry points are safe to call from a single non-GUI thread
- Concurrent calls on one `ExecutionEngine` are **not** supported in the MVP
- The desktop marshals work with `QtConcurrent::run` + `QFutureWatcher` and
  crosses back to the GUI thread before touching UI state

## Attacker-controlled surfaces

Two inputs come from outside and are treated as hostile:

| Surface | Where | Guards |
| --- | --- | --- |
| Schema files | `YamlSchemaParser` | 8 MiB file cap, 64-deep nesting cap |
| Imported specs | `ImportFromOpenApi` | Path canonicalisation, containment root |

Related guards elsewhere: hook scripts must be relative, inside the project root,
and under 1 MiB; uploads cap at 50 MiB; secrets are read individually rather than
dumped.

## Stack

| Choice | Version | Why |
| --- | --- | --- |
| C++ | **23** | `std::expected`, `std::print`, ranges. C++26 is not portable yet |
| CMake | **4.0+** | |
| Qt | **6.8 LTS** | Installed via aqtinstall, not built from source |
| Dependencies | vcpkg manifest mode | Everything except Qt |
| Tests | GoogleTest | 852 tests |

## Where to look in the source

| To understand | Read |
| --- | --- |
| Accepted schema keys | `engine/src/infrastructure/schema/YamlSchemaParser.cpp` |
| The `{{...}}` grammar | `engine/src/domain/VariableResolver.cpp` |
| Chain building | `engine/src/domain/DependencyResolver.cpp` |
| The error taxonomy | `engine/include/reqloom/engine/ErrorCodes.h` |
| Public API surface | `engine/include/reqloom/engine/PublicApi.h` |
| CLI output shapes | `cli/src/output/` |

## Next

- [Building from source](/dev/building/) — presets, sanitizers, tests
- [Contributing](/dev/contributing/) — conventions and review process
- [Roadmap](/dev/roadmap/) — what's done and what's missing
