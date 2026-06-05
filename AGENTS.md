# AGENTS.md — Reqloom

Project-level instructions for AI coding agents (Kiro, Codex, Claude Code, Cursor, etc.). Keep terse. The full product spec lives in `doc/`.

## Context

**Reqloom** is a workflow-aware API testing tool. Treats an API as a graph of resources, actors, and dependencies; auto-resolves request chains. Three deliverables:

- **`engine/`** — `libreqloom-engine`, a pure C++ engine library. **No Qt UI dependency.**
- **`cli/`** — `reqloom` CLI binary. Links engine + `Qt6::Core` only.
- **`desktop/`** — Qt 6 desktop UI app. The *only* place Qt UI libraries are allowed.
- **`ipc/`** — Phase B scaffold (currently empty).

Read [`doc/Reqloom - Project Layout.md`](doc/Reqloom%20-%20Project%20Layout.md) before making structural changes.

## Stack Baseline

| Tool | Pinned to |
|---|---|
| CMake | **4.0** minimum |
| C++ standard | **C++23** |
| Qt | **6.8 LTS** minimum |
| Compiler | Apple Clang 16+, Clang 18+, GCC 14+, MSVC 19.40+ |
| Dependency manager | **vcpkg** (manifest mode, `master` baseline) for non-Qt deps; **aqtinstall** for Qt 6.8 LTS |
| Test framework | GoogleTest |
| CI | GitHub Actions (Linux + Windows, public-repo free runners) and Azure DevOps Pipelines (macOS). |

C++26 is not portable yet; do not use C++26-only features (reflection, contracts, `std::execution`) without feature-test gating. Use C++23 idioms (`std::expected`, `std::print`, ranges additions, `deducing this`) freely — they are stable on all supported compilers.

## Architectural Boundary (Hard Rule)

The engine layer must not directly or transitively depend on Qt UI or the QScintilla code editor. Enforced in three places:

1. **CMake link guard** — `cmake/ReqloomBoundaryGuards.cmake` fails configure if the engine, CLI, or engine tests link `Qt6::Widgets`, `Qt6::Gui`, `Qt6::Quick`, `Qt6::QuickWidgets`, or `QScintilla::QScintilla`.
2. **CI grep job** — `.github/workflows/boundary-check.yml` rejects any `#include <QWidget>` / `<QApplication>` / `<Qsci...>` under `engine/` or `cli/`.
3. **Public header surface** — `engine/include/reqloom/engine/*.h` uses pImpl + value types. No Qt UI types appear.

If you need to expose engine state to the desktop UI, do it via `engine/include/reqloom/engine/Events.h` callbacks or expand `PublicApi.h`. Do not add a `reqloom-ui-shared` target that imports both worlds.

## Build & Test Commands

```bash
# Configure & build (macOS Debug with ASan + UBSan; uses Homebrew Qt)
cmake --preset macos-debug
cmake --build --preset macos-debug

# Run all tests
ctest --test-dir build/macos-debug -j $(sysctl -n hw.ncpu) --output-on-failure

# Engine tests only (fastest)
ctest --test-dir build/macos-debug -j $(sysctl -n hw.ncpu) --label-regex engine

# Run the CLI
./build/macos-debug/cli/reqloom --help

# Run the desktop app
./build/macos-debug/desktop/Reqloom.app/Contents/MacOS/Reqloom

# Pre-push smoke check (configure + build + tests + boundary check)
./tools/pre-push-check.sh
```

Other presets: `macos-release`, `linux-debug`, `linux-release`, `windows-debug`, `windows-release`.

**Qt source.** Qt 6.8 LTS is installed out-of-band via [`aqtinstall`](https://github.com/miurahr/aqtinstall), not vcpkg. Building qtbase from source via vcpkg added 45-90 minutes per cold-cache CI run, which exceeded AppVeyor's 60-minute job cap. The pre-built Qt comes from the official Qt download mirror — same artifacts the Qt online installer uses — and is signature-checked by aqtinstall.

For local development:

```bash
# macOS / Linux
./tools/setup-qt.sh
export CMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos"   # or .../gcc_64 on Linux

# Windows (cmd.exe)
tools\setup-qt.cmd
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
```

CI does the equivalent in `.github/workflows/build.yml` (Linux + Windows) and `azure-pipelines.yml` (macOS). Both pin `QT_VERSION` — keep it in sync with `tools/setup-qt.sh`.

**Pre-push hook.** Run `git config core.hooksPath tools/git-hooks` once to wire up the pre-push smoke check. Bypass with `git push --no-verify` when justified.

## Code Style

- **Formatter**: `clang-format` per `.clang-format`. Run `tools/format.sh` before committing.
- **Lint**: `clang-tidy` per `.clang-tidy`.
- **Warnings**: `-Wall -Wextra -Wpedantic -Wconversion`, applied via `reqloom_set_warnings()`. `-Werror` is gated behind CI, not local builds.
- **File size**: ≤ 800 lines; ≤ 50 lines per function. Split rather than scroll.
- **Headers**: forward-declare in `.h`, include in `.cpp`. One class per public header.
- **Error handling**: prefer `std::expected<T, E>` (C++23) over error-code globals or out-parameters. Reserve exceptions for genuinely unrecoverable conditions; never throw across ABI boundaries or in destructors.

## Hard Rules — Write Code That Doesn't Trigger Lint Warnings

These rules exist because clang-tidy and the project warning set generate hundreds of warnings on AI-written C++ that ignores them. Apply these as you write, not after. The reviewer hooks will reject diffs that violate them.

### Always

- **Always brace control flow.** Every `if`, `else`, `for`, `while`, `do` gets `{}` even if the body is one line. No `if (x) doFoo();`. Reduces patch errors and matches the project's `.clang-format`.
- **Always declare enum size.** `enum class Foo : std::uint8_t { ... }` unless a wider type is genuinely needed. The narrow type catches truncation bugs and is required by `cppcoreguidelines-explicit-virtual-functions`.
- **Always brace-initialize.** `int x{};`, `std::string s{};`, never `int x;` for fundamental types. Avoids C++26 erroneous-behavior warnings on uninitialized reads.
- **Always `const` member functions** that don't mutate. Always `const` parameters that aren't moved-from.
- **Always `noexcept`** on move ctor / move assign / `swap` / destructors. Without it, `std::vector<T>` falls back to copy on growth — silent perf regression that clang-tidy flags as `performance-noexcept-move-constructor`.
- **Always `[[nodiscard]]`** on:
  - Functions returning `std::expected`, `std::optional`, `std::unique_ptr`.
  - Pure / query functions where ignoring the return value is a bug (e.g. `RunContext::session(id)`).
- **Always `explicit`** on single-arg constructors (and conversion operators) unless implicit conversion is intentional. Without it, `modernize-use-explicit` fires.
- **Always include what you use.** If you use `std::string`, include `<string>`. The recent `ErrorCodes.h` bug shipped because `<string>` was missing — clang-tidy `misc-include-cleaner` would have caught it.
- **Always close namespaces with a comment.** `}  // namespace reqloom::engine` — required by `google-readability-namespace-comments`.

### Never

- **Never use `std::endl`.** Use `'\n'` or `std::println`. Forces flush and dominates `performance-avoid-endl` warnings.
- **Never use C-style casts** `(int)x`. Use `static_cast`, `dynamic_cast`, `reinterpret_cast`, `const_cast`, or `std::bit_cast`. Trips `cppcoreguidelines-pro-type-cstyle-cast`.
- **Never use `NULL`.** Use `nullptr`.
- **Never use `typedef`.** Use `using Foo = ...;`.
- **Never use plain `enum`** (without `class`). Implicit-int conversion is a hazard.
- **Never use raw `new` / `delete` outside RAII wrappers.** `std::make_unique` / `std::make_shared`.
- **Never use `printf`-family.** Use `std::print` / `std::println` (C++23) or `std::format`.
- **Never write a manual range-for over indices** when `<ranges>` works. `for (auto& x : views::enumerate(v))` over `for (size_t i = 0; i < v.size(); ++i)`.
- **Never use `<bits/stdc++.h>`.** Non-portable; Apple Clang doesn't ship it.
- **Never use `using namespace std;` at namespace or header scope.** OK *inside* a function body for brevity.
- **Never use `goto`.** No exceptions on this project.
- **Never use `-DNDEBUG` to silence `assert`** — change the assert.

### Strongly prefer

- **Pass by value + move** for sink parameters; **`const T&`** for big read-only; **value** for cheap-to-copy.
- **`emplace_back`** over `push_back(T(...))`.
- **`reserve`** before known-size insertion loops.
- **`const auto&`** in range-for loops to avoid copies.
- **`std::array<T, N>`** over C arrays.
- **`std::span<T>`** over `T*` + length pair for non-owning views.
- **`std::string_view`** for non-owning string parameters; **`std::string`** when sinking.

### Specific to this project

- **`Q_OBJECT`** must appear in any class deriving from `QObject` that uses signals/slots. Forgetting it makes signals silently unreachable.
- **`Q_PROPERTY` setters** must early-return if value didn't change, then emit the `NOTIFY` signal — otherwise QML binding loops.
- **`connect()`** in function-pointer form only: `connect(a, &A::sig, b, &B::slot)`. String form `SIGNAL()/SLOT()` is banned.
- **Lambda passed to `connect()`** must include a receiver `QObject*` so it auto-disconnects on destruction.
- **All public engine API returns** `std::expected<T, ReqloomError>`. Add a code to `ErrorCode` rather than a parallel error enum.
- **All `target_link_libraries`** use `PUBLIC` / `PRIVATE` / `INTERFACE` keywords explicitly. Plain forms are banned.

## Naming Conventions

- Files: `PascalCase.{h,cpp}` (matches Qt convention used in this repo).
- Types: `PascalCase`. Functions and methods: `camelCase`. Constants: `kCamelCase` or `SCREAMING_SNAKE_CASE` for macros only.
- Namespace: `reqloom::engine` for engine types, `reqloom::cli`, `reqloom::desktop` for app code.
- CMake targets: `reqloom-<layer>` (`reqloom-engine`, `reqloom-cli`, `reqloom-desktop`). Aliases: `reqloom::engine`.

## Comments & Documentation

### Philosophy

- **Comments explain *why*, not *what*.** The code tells you what happens; the comment tells you why that decision was made, what invariant is being upheld, or what the non-obvious constraint is.
- **If you need a comment to explain *what* code does, the code is too clever.** Simplify the code first.
- **No trailing comments.** Comments go above the line or block they describe.
- **No "change log" comments** (`// Added by X on Y`, `// TODO: remove after sprint 4`). That's what git blame is for.

### Required comments

| Where | What | Example |
|---|---|---|
| Top of every `.h` and `.cpp` | One-line purpose + spec ref | `// Per-run mutable state. Engine Requirement §3.3.` |
| `namespace` close brace | Namespace name | `}  // namespace reqloom::engine` |
| Non-obvious algorithm | Brief rationale + complexity | `// Kahn's algorithm — O(V+E), avoids recursion for large graphs.` |
| `NOLINT` / `NOLINTNEXTLINE` | Why the suppression is justified | `// NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): required for C interop with sqlite3` |
| Public API functions in headers | `///` doc-comment: brief, params, return, errors | See "Doxygen style" below |
| Magic numbers or constants | What the value means | `constexpr int kMaxYamlDepth = 64;  // prevent stack overflow on malicious input` |
| Preprocessor guards | Condition description when non-trivial | `#ifdef REQLOOM_HAS_QTKEYCHAIN  // vcpkg provides this on Linux/Windows; macOS uses Security.framework directly` |

### Forbidden comments

- **Commented-out code.** Delete it; git has history. If you're keeping it as a reference for a future approach, move it to a doc or an issue.
- **Obvious restating.** `// increment counter` above `++counter;` — adds noise, not value.
- **End-of-line `// ...`** on the same line as code. Put the comment on the line above.
- **Block-separator banners** in the middle of a function (`// ====== STEP 2 ======`). If a function needs section headers, it's too long — split it.
- **Attribution / authorship.** `// Written by Mirza, May 2026` — that's git blame territory.
- **Aspirational TODOs without an issue link.** `// TODO: fix this someday` is dead weight. Write `// TODO(#42): handle timeout retry per Engine Requirement §3.5` or don't write a TODO.

### Doxygen style (public headers only)

Use `///` (triple-slash) style, not `/** */` blocks. Keep it tight.

```cpp
/// Resolve the full dependency chain for a target operation.
///
/// Walks the resource graph (Kahn's topological sort) and returns an
/// ordered list of operations to execute, including authentication steps.
///
/// @param target  The operation the user clicked / invoked.
/// @param ctx     Current run context (session cache, extraction state).
/// @return Ordered execution plan, or ReqloomError on cycle / missing ref.
///
/// @note Thread-safety: not re-entrant. Caller must serialize access.
std::expected<ExecutionPlan, ReqloomError>
resolve(const OperationId& target, const RunContext& ctx);
```

Rules:

- First line is a single sentence summary ending with a period.
- Blank line between summary and extended description.
- `@param`, `@return`, `@note`, `@throws` — only when non-obvious.
- Don't document getters/setters unless the semantic is surprising.
- Don't document private/internal functions with Doxygen — a plain `//` one-liner above is enough.

### Comments in tests

- **Test name is the primary documentation.** `TEST(DependencyResolver, returns_cycle_error_when_graph_has_back_edge)` should need zero comments.
- If the Arrange block is non-trivial, a one-line comment above it explaining the scenario is fine: `// Construct a graph with A→B→C→A`.
- Never comment the Assert block with `// should be true` — if the assertion fails, GoogleTest prints the actual vs expected. That's the comment.

### CMake comments

- One-line comment above each `add_library` / `add_executable` explaining what the target is.
- Comments above `target_link_libraries` blocks explaining *why* each dep is needed (especially for non-obvious ones like `Qt6Keychain`).
- Never comment `#` at end of line in CMake — put it on the line above.

### AI-generated code: comment hygiene

When an AI agent produces code, apply these filters before accepting:

- **Delete any "Here's what this does" explanatory comment that restates the code.** Models over-explain to prove they understand; the result is noisy.
- **Keep any "This is needed because..." comment that explains a constraint.** That's genuine value.
- **Rewrite any vague TODO** to include an issue number or a specific condition under which the TODO should be addressed.
- **Remove any `// Added by AI` or `// Generated` markers.** They're noise and git blame already records the commit.

## Layer Rules (Clean Architecture)

```
domain/        ← pure business logic, no I/O. Allowed deps: stdlib + Qt6::Core (for QString only).
application/   ← use cases. Allowed deps: domain + infrastructure (privately).
infrastructure/← I/O adapters (HTTP, SQLite, secrets, schema). Concrete impls behind interfaces.
```

Public engine headers (`engine/include/reqloom/engine/`) contain only domain + application surfaces. Infrastructure types are private.

## Coding Conventions (Reference)

The following patterns are the project's conventions. Match them when adding new code; reviewers reject anything that diverges without justification.

### Header skeleton

Every header starts with a one-line comment naming the type's purpose (and the spec section if applicable), `#pragma once`, then includes ordered: project, third-party, standard.

```cpp
// engine/include/reqloom/engine/Foo.h
// Public-facing value type. Engine Requirement §X.Y.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <string>
#include <vector>

namespace reqloom::engine {

class Foo {
public:
    Foo();
    ~Foo();
    Foo(const Foo&) = delete;
    Foo& operator=(const Foo&) = delete;
    Foo(Foo&&) noexcept;
    Foo& operator=(Foo&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reqloom::engine
```

### Error handling

The canonical error type is `reqloom::engine::ErrorCode` (see `engine/include/reqloom/engine/ErrorCodes.h`). Wrap recoverable failures in `std::expected<T, ErrorCode>`:

```cpp
#include <reqloom/engine/ErrorCodes.h>
#include <expected>

std::expected<RunResult, ErrorCode>
RunOperationUseCase::execute(const OperationId& op, RunContext& ctx) {
    auto resolved = resolver_.resolve(op, ctx);
    if (!resolved) return std::unexpected(resolved.error());

    auto response = http_->send(*resolved);
    if (!response) return std::unexpected(map_network_error(response.error()));

    return RunResult{*response};
}
```

Rules:

- **Never** introduce a parallel error enum. Add a code to `ErrorCode` instead — it's the QA contract.
- Use `to_code_string(code)` when surfacing the code in user-facing output.
- Use `is_retryable(code)` to decide retry behaviour, not ad-hoc `code == ...` checks.
- Throw exceptions only at the engine boundary for unrecoverable, non-resumable conditions (out-of-memory, invariant violation). Never across the C ABI; never in destructors.

### Defining a new infrastructure adapter

1. Pure interface in `engine/src/infrastructure/<area>/<Name>.h`:

   ```cpp
   #pragma once
   #include <expected>
   #include <reqloom/engine/ErrorCodes.h>

   namespace reqloom::engine {

   class HookRunner {
   public:
       virtual ~HookRunner() = default;
       virtual std::expected<HookResult, ErrorCode>
           run(std::string_view source, const RunContext& ctx) = 0;
   };

   }
   ```

2. Concrete impl in `engine/src/infrastructure/<area>/<Concrete>{.h,.cpp}` — never on the public include path.
3. Add the `.cpp` to `engine/CMakeLists.txt` under `reqloom-engine-infrastructure`.
4. If the adapter introduces a new engine sub-target, append it to the `reqloom_forbid_dependencies(...)` loop in `engine/CMakeLists.txt`.

### Defining a new use case

1. Header in `engine/src/application/<Name>UseCase.h` (private — not under `include/`).
2. Constructor takes interfaces, not concretes:

   ```cpp
   class RunBatchUseCase {
   public:
       RunBatchUseCase(DependencyResolver& resolver,
                       RunOperationUseCase& runner)
           : resolver_(resolver), runner_(runner) {}

       std::expected<BatchResult, ErrorCode>
           execute(std::span<const OperationId> ops, RunContext& ctx);

   private:
       DependencyResolver& resolver_;
       RunOperationUseCase& runner_;
   };
   ```

3. Wire it in `ExecutionEngine::Impl` so the public surface stays narrow.

### RunContext discipline

`RunContext` is move-only and lives the lifetime of one run. Inside the engine:

- Pass by `RunContext&` to use cases that mutate it; `const RunContext&` to readers.
- Never copy. Never store a `RunContext*` past the call returning it.
- Record every step with `ctx.record(StepResult{...})` — the timeline panel and JUnit renderer depend on it.

### Threading

- The engine does not pin work to threads. Callers (CLI / desktop) decide.
- All public engine entry points must be safe to call from a single non-GUI thread; concurrent calls on the same `ExecutionEngine` are not supported in the MVP.
- The desktop app marshals work with `QtConcurrent::run` + `QFutureWatcher`; never call engine APIs from a Qt slot bound to a worker thread without first crossing back to the GUI thread.

### Logging

- Log via the engine's logger (Engine Requirement §10), not `qDebug` and not `std::cout`.
- Categories use the `reqloom.<area>` namespace: `reqloom.http`, `reqloom.schema`, `reqloom.run`.
- **Never log secrets, full request bodies, or full response bodies.** Log first 64 bytes of bodies max; redact `Authorization`, `Cookie`, `X-API-Key`, and any header containing `token` or `secret`.

### Reading user input

The two attacker-controlled surfaces are:

- `engine/src/application/ImportFromOpenApi.cpp` — URLs and file paths from CLI/desktop.
- `engine/src/infrastructure/schema/YamlSchemaParser.cpp` — `reqloom.yaml` content.

Rules for both:

- **No string concatenation into shell, exec, or system calls.** Use `QProcess` with an argument list or `posix_spawn`.
- **Path inputs**: canonicalise with `std::filesystem::weakly_canonical`, then verify the result is under the project root. Reject `..` traversal and absolute paths unless explicitly allowed.
- **URL inputs**: parse with `QUrl`, reject schemes other than `http(s)`/`file` per a fixed allowlist, reject IP-literal hosts unless explicitly allowed.
- **YAML/JSON**: cap depth and document size before parsing.

### Test skeleton

```cpp
// engine/tests/unit/RunBatchUseCaseTest.cpp
#include <gtest/gtest.h>
#include <reqloom/engine/ErrorCodes.h>
#include "application/RunBatchUseCase.h"

namespace reqloom::engine::tests {

TEST(RunBatchUseCase, fails_with_cycle_when_topology_invalid) {
    // Arrange — fake resolver that reports a cycle
    FakeResolver resolver{ResolverFault::Cycle};
    FakeRunner   runner;
    RunBatchUseCase uc{resolver, runner};
    RunContext ctx;

    // Act
    const auto result = uc.execute({{"a"}, {"b"}}, ctx);

    // Assert
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::Cycle);
    EXPECT_EQ(ctx.steps().size(), 0u);
}

}  // namespace reqloom::engine::tests
```

Conventions:

- File name mirrors the unit under test.
- Test name is `<unit>_<observed_behavior>` in `snake_case`. The verb is what *was observed*, not what *should* happen.
- Arrange / Act / Assert blocks separated by blank lines.
- Tests use real engine types (`RunContext`, `ErrorCode`); only I/O dependencies are faked.
- A bug fix lands with a test that fails on the parent commit and passes on the fix commit.

### Source file template

```cpp
// engine/src/<layer>/<Name>.cpp
// Brief one-line description. Spec ref if any.

#include "<Name>.h"

#include <reqloom/engine/ErrorCodes.h>
#include <expected>

namespace reqloom::engine {

// implementation

}  // namespace reqloom::engine
```

### CMake addition checklist

When adding a new `.cpp` to the engine:

- [ ] Add the file path to the right `add_library(reqloom-engine-<layer> OBJECT ...)` block in `engine/CMakeLists.txt`.
- [ ] Update `target_link_libraries` only if the file pulls in a new third-party.
- [ ] If introducing a new third-party, add it to `vcpkg.json` and re-pin `vcpkgGitCommitId` if necessary.
- [ ] Confirm `reqloom_forbid_dependencies(...)` still covers the layer.
- [ ] Add a unit test under `engine/tests/unit/` with matching `gtest_discover_tests` registration.

## Testing Expectations

- New code lands with tests. Domain-layer tests aim for 90%+ coverage.
- Use real dependencies via test fixtures (sqlite in-memory, mock SUT under `tests/fixtures/mock-sut/`). Never mock the database.
- Each test independent. No shared mutable state across tests.
- Tests live next to the code: `engine/tests/unit/`, `engine/tests/integration/`.

Add a corresponding test for every bug fix that reproduces the failure first.

## Git & PRs

- Conventional commits: `feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`, `perf:`, `ci:`. Subject ≤ 70 chars.
- Branch from `main`. Push to a feature branch. Open a PR; never push directly to `main`.
- PR description must list what was tested. Failing CI blocks merge.
- Do not commit secrets, build outputs (`build/`), or vcpkg installed trees. `.gitignore` already covers these — verify your staging area before committing.


### Hard rules for AI-assisted edits

1. **One function or one file per turn.** Not one feature. Multi-file features go through `/planner` first, then land in separate commits.
2. **Compile before claiming done.** No edit is "complete" until `cmake --build --preset macos-debug` returns 0 for the affected target. The `cpp-build-on-edit` hook makes this automatic on save.
3. **Run the targeted test before claiming done.** `ctest --preset macos-debug --label-regex engine -R <YourTest>`. Models often write code that compiles but fails the test it was supposed to satisfy.
4. **Never accept code you didn't see built.** A model that says "this should compile" is admitting it didn't try.
5. **No C++26 features.** Reflection (`^^T`), contracts (`pre`/`post`), `std::execution`, `std::inplace_vector`, `std::hive` — banned until compiler support stabilises across all three CI runners. Stay in C++23.
6. **`/cpp-build-resolver` is the default for build, link, and template errors** — not `/cpp-reviewer`. Reviewers grade quality; resolvers unblock. Do not ask `/cpp-reviewer` to "fix the build".
7. **`/qt-reviewer` is the default for any change under `desktop/`** — even a one-line tweak. Qt's parent-ownership and signal/slot rules trip every model.
8. **Reject hallucinated APIs.** If a model uses a Qt or stdlib symbol you don't recognise, look it up in cppreference or the Qt 6.8 docs *before* keeping the change. Common fabrications: `QObject::deleteLater(int)`, `std::expected::and_then` overloads that don't exist, `Qt::ConnectionType` values that were renamed.
9. **Tests must fail without the change.** Run the test on the parent commit; if it passes there, the test does not exercise the new code. AGENTS.md already states this — restating because models routinely produce green tests that test nothing.

### Failure modes specific to this repo

A non-exhaustive list of mistakes models make repeatedly here is documented in [`doc/Cpp-Failure-Modes.md`](doc/Cpp-Failure-Modes.md). Skim it before asking an agent to write engine or desktop code; reread it after a confusing model error.

### Project hooks (auto-active)

Two file-save hooks run automatically on `*.cpp` / `*.h` saves:

- **`cpp-build-on-edit`** — runs `tools/build-incremental.sh`, which builds the appropriate preset for your OS (`macos-debug`, `linux-debug`, or `windows-debug`). Skips cleanly with a hint if the preset hasn't been configured yet — set it up once with `cmake --preset <preset>`. Override the preset with `$REQLOOM_PRESET`.
- **`clang-tidy-on-edit`** — runs clang-tidy on the changed file using `.clang-tidy`. Reports `modernize-*`, `bugprone-*`, `performance-*`, `cert-*`, `cppcoreguidelines-*` findings only. Bails cleanly if the build directory or clang-tidy is missing.

Toggle them in the Agent Hooks panel if a long compile blocks edit flow. Don't disable them silently before a PR.

## Available Agents (Kiro / Compatible Harnesses)

When working on this codebase, prefer these specialised agents over generic flow:

- **`/cpp-build-resolver`** — **first stop for any build, link, CMake, template, or sanitizer error.** Minimal-diff fixes; will not refactor.
- **`/cpp-reviewer`** — UB, ownership, concurrency, and modern C++ quality review. Run *after* the build is green.
- **`/qt-reviewer`** — mandatory for any `desktop/` change. QObject lifetime, signal/slot correctness, QML/C++ bridge.
- **`/security-reviewer`** — input validation, deserialization, secret handling. Use when touching the import path, hook runner, or secret store.
- **`/vuln-validator`** — adversarial review of an existing security finding. Confirms or refutes; cannot file new findings.
- **`/reachability-tracer`** — given a confirmed bug, walks taint backward to decide if attacker-controlled input can reach it.
- **`/planner`** — break down complex changes before coding. Use for any multi-file feature.

Skills:

- **`/cpp-patterns`** — modern C++ idioms reference (C++23 only — see rule 5 above).
- **`/qt-patterns`** — Qt 6.8 idioms and CMake integration.
- **`/hunt`** — narrow-scope vulnerability hunting (one attack class × one component).

## Things to Avoid

- Adding a `find_package(Qt6 ...)` call for `Widgets`, `Gui`, or `Quick` outside `desktop/CMakeLists.txt`.
- `#include <QWidget>` (or any UI-namespaced Qt header) under `engine/` or `cli/`.
- `file(GLOB ...)` for source lists in any `CMakeLists.txt`.
- Bumping the public API surface without updating `engine/include/reqloom/engine/PublicApi.h`.
- Using `using namespace std;` or `using namespace reqloom;` at namespace or header scope.
- Catching exceptions just to log and rethrow without context. Use `std::expected` in new code.
- Adding agent-generated tests that don't actually exercise the change. Each test must fail without the code under test.
- **C++26-only features**: `^^T` reflection operator, `pre`/`post`/`contract_assert`, `std::execution` / sender-receiver, `std::inplace_vector`, `std::hive`, `=delete("reason")`. Apple Clang 16 / Clang 18 / GCC 14 / MSVC 19.40 do not support these uniformly. If you're tempted, the answer is no until 2027.
- **Suggesting a refactor while fixing a build error.** `/cpp-build-resolver`'s job is the smallest diff that compiles. Quality changes go through a separate `/cpp-reviewer` pass.
- **Editing more than one `.cpp` per agent turn for engine code.** Multi-file changes need `/planner` first, then individual edits.

## Things Worth Doing Proactively

- Use `std::expected<T, ReqloomError>` for new error paths in the engine application layer.
- Use `std::print` / `std::println` instead of `iostream` chains for new code.
- Use C++23 ranges additions (`views::zip`, `views::enumerate`, `ranges::fold_left`, `ranges::to`) where they replace explicit loops.
- When adding a new infrastructure adapter, define the pure interface in `engine/src/infrastructure/<area>/<Name>.h` first, then the concrete `.cpp`.
- Add the matching `reqloom_forbid_dependencies(...)` call when introducing a new engine sub-target.

## License & Scope

Apache 2.0. The OSS license covers engine, CLI, schema, and desktop app. The AI importer (`prompts/import/`) and the planned team-workspace cloud sync are designated for closed paid components — do not commit closed-source-only code under the OSS tree without first updating `LICENSE` and PRD §15.
