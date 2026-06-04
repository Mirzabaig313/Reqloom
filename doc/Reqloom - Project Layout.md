# Reqloom — Project Layout & CMake Structure

> **Purpose:** define the source-tree layout, CMake target structure, and dependency boundaries that enforce the architectural guardrails in PRD §8.6 (Two-Phase Architecture). This document is the literal contract between the engine and the UI: if these rules hold, extracting the engine to a separate process or language in Phase B becomes a build-system change rather than a rewrite.

| | |
|---|---|
| **Status** | Draft v1 |
| **Last Updated** | 2026-05-23 |
| **Source PRD** | `Reqloom - PRD.md` v0.3 (§8) |
| **Source Engine Spec** | `Reqloom - Engine Requirement.md` v1.1 |

---

## 1. Top-Level Source Tree

```
reqloom/
├── CMakeLists.txt                    # Root: superbuild, options, top-level targets
├── CMakePresets.json                 # Configure/build/test presets per platform
├── conanfile.txt                     # Or vcpkg.json — pinned third-party deps
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── README.md
├── LICENSE                           # Apache-2.0 (engine, CLI) / source-available for closed UI parts
├── docs/                             # PRD, engine spec, ADRs, this file
│
├── cmake/                            # Reusable CMake modules
│   ├── ReqloomCompilerWarnings.cmake
│   ├── ReqloomSanitizers.cmake     # ASan/UBSan/TSan toggles
│   ├── ReqloomCodeCoverage.cmake
│   ├── ReqloomBoundaryGuards.cmake # The dependency-firewall enforcement (§4)
│   └── packaging/
│       ├── macOS.cmake               # bundle, codesign, notarize
│       ├── Windows.cmake             # MSI/EXE, Authenticode
│       └── Linux.cmake               # AppImage, .deb, .rpm
│
├── third_party/                      # Vendored if needed; otherwise managed via Conan/vcpkg
│   └── README.md                     # Why each dep is vendored
│
├── engine/                           # ── ENGINE LAYER (no Qt UI deps) ──────────────────
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── reqloom/
│   │       └── engine/
│   │           ├── ExecutionEngine.h
│   │           ├── DependencyResolver.h
│   │           ├── RunContext.h
│   │           ├── Operation.h
│   │           ├── Actor.h
│   │           ├── Resource.h
│   │           ├── ErrorCodes.h
│   │           ├── Events.h           # Observability events from §10 of engine spec
│   │           └── PublicApi.h        # Single header for embedders
│   ├── src/
│   │   ├── domain/                   # Pure business logic — no I/O
│   │   │   ├── DependencyResolver.cpp
│   │   │   ├── Topology.cpp
│   │   │   ├── VariableResolver.cpp
│   │   │   ├── RunContext.cpp
│   │   │   └── …
│   │   ├── application/              # Use cases that orchestrate domain + infra
│   │   │   ├── RunOperationUseCase.cpp
│   │   │   ├── ImportFromOpenApi.cpp
│   │   │   └── …
│   │   ├── infrastructure/           # I/O adapters; engine-internal only
│   │   │   ├── http/
│   │   │   │   ├── CurlHttpClient.cpp
│   │   │   │   └── HttpClient.h     # Pure interface
│   │   │   ├── storage/
│   │   │   │   ├── SqliteHistoryStore.cpp
│   │   │   │   └── HistoryStore.h   # Pure interface
│   │   │   ├── secrets/
│   │   │   │   ├── KeychainSecretStore.cpp     # Cross-platform via QtKeychain
│   │   │   │   └── SecretStore.h    # Pure interface
│   │   │   ├── schema/
│   │   │   │   ├── YamlSchemaParser.cpp        # yaml-cpp
│   │   │   │   └── SchemaParser.h   # Pure interface
│   │   │   ├── hooks/
│   │   │   │   ├── QuickJsHookRunner.cpp
│   │   │   │   └── HookRunner.h     # Pure interface
│   │   │   └── extraction/
│   │   │       ├── JsonPathEvaluator.cpp
│   │   │       └── …
│   │   └── plumbing/                 # Logging, error mapping, threading primitives
│   │       ├── Logger.h
│   │       ├── ErrorMapper.cpp
│   │       └── ThreadPool.cpp
│   └── tests/
│       ├── unit/
│       │   ├── DependencyResolverTests.cpp
│       │   ├── VariableResolverTests.cpp
│       │   └── …
│       ├── integration/              # Engine + real HTTP via mock SUT
│       │   ├── HappyPathTests.cpp
│       │   ├── SessionLifecycleTests.cpp
│       │   ├── CancellationTests.cpp
│       │   └── …                     # Mirrors §8 of Engine Requirement
│       └── fixtures/
│           ├── sample-marketplace/   # The MarketplaceAPI sample schema
│           └── mock-sut/             # Tiny local server for integration tests
│
├── ipc/                              # ── PHASE B SCAFFOLD (empty in MVP, reserved) ──
│   ├── CMakeLists.txt                # Disabled by default; REQLOOM_BUILD_IPC=ON to opt in
│   └── README.md                     # "When and how to extract engine to a separate process"
│
├── cli/                              # ── CLI BINARY (FR-13) — depends on engine only ──
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── commands/
│   │   │   ├── RunCommand.cpp
│   │   │   ├── LintCommand.cpp
│   │   │   └── ImportCommand.cpp
│   │   └── output/
│   │       ├── TextRenderer.cpp
│   │       ├── JsonRenderer.cpp
│   │       └── JUnitRenderer.cpp
│   └── tests/
│
├── desktop/                          # ── QT UI APP — depends on engine + ui-shared ──
│   ├── CMakeLists.txt
│   ├── resources/                    # icons, qrc, translations
│   ├── src/
│   │   ├── main.cpp
│   │   ├── application/
│   │   │   ├── App.cpp
│   │   │   └── Bootstrapper.cpp     # Constructs ExecutionEngine, wires to view models
│   │   ├── viewmodels/               # MVVM glue between engine and Qt views
│   │   │   ├── ProjectExplorerViewModel.{h,cpp}
│   │   │   ├── RunViewModel.{h,cpp}
│   │   │   ├── ResponseViewModel.{h,cpp}
│   │   │   └── EnvironmentViewModel.{h,cpp}
│   │   ├── views/                    # Qt Widgets
│   │   │   ├── ProjectExplorerWidget.{h,cpp,ui}
│   │   │   ├── RequestEditorPanel.{h,cpp,ui}
│   │   │   ├── ResponseViewerPanel.{h,cpp,ui}
│   │   │   ├── TimelinePanel.{h,cpp,ui}
│   │   │   ├── CodeEditor.{h,cpp}    # QScintilla wrapper for YAML/JSON/JS
│   │   │   └── DependencyGraphView.{h,cpp}  # QQuickWidget host for QML graph
│   │   ├── widgets/                  # Reusable atoms (StatusBadge, JsonTree, etc.)
│   │   ├── theming/
│   │   │   ├── Theme.{h,cpp}
│   │   │   ├── light.qss
│   │   │   └── dark.qss
│   │   ├── shortcuts/
│   │   │   └── ShortcutRegistry.cpp  # Cmd-Enter / Cmd-P / Esc per §9.3
│   │   └── platform/                 # OS-specific glue (menus, dock, file watchers)
│   │       ├── macos/
│   │       ├── windows/
│   │       └── linux/
│   ├── qml/
│   │   └── DependencyGraph.qml
│   └── tests/
│       └── ui/                       # Qt Test framework — view-level only
│
├── ui-shared/                        # Optional: helpers used by desktop only (not engine)
│   └── …
│
├── samples/                          # Bundled sample projects (PRD §12 first-run)
│   ├── marketplace/
│   │   ├── reqloom.yaml
│   │   ├── actors/
│   │   ├── resources/
│   │   └── environments/
│   └── …
│
├── prompts/                          # AI importer prompt templates (PRD §10)
│   └── import/
│       ├── system.md
│       └── few-shot/
│
└── tools/
    ├── format.sh                     # clang-format wrapper
    ├── lint.sh                       # clang-tidy wrapper
    └── ci/
        ├── boundary-check.cmake      # CI-only verification of dependency rules
        └── …
```

---

## 2. CMake Target Structure

The crucial idea: **every directory above a certain line is forbidden from depending on Qt UI libraries**. We make this physical by giving each module its own CMake target with explicit `target_link_libraries(... PRIVATE ...)` calls.

### 2.1 Targets and their allowed dependencies

```
                 ┌─────────────────────────────────────────────┐
                 │             reqloom-desktop                │   ← Qt::Widgets, Qt::Gui, Qt::Quick, QScintilla
                 │              (executable)                   │     ─ may use reqloom-engine
                 └──────────────────────┬──────────────────────┘
                                        │
                                        ▼
                 ┌─────────────────────────────────────────────┐
                 │              reqloom-cli                   │   ← Qt::Core ONLY (for QString convenience)
                 │              (executable)                   │     ─ may use reqloom-engine
                 └──────────────────────┬──────────────────────┘     ─ MUST NOT use Qt::Widgets/Gui/Quick
                                        │
                                        ▼
                 ┌─────────────────────────────────────────────┐
                 │           reqloom-engine                   │   ← Qt::Core (or std-only — preferred)
                 │       (STATIC or SHARED library)            │     ─ MUST NOT use Qt::Widgets/Gui/Quick
                 │                                             │     ─ MUST NOT use QScintilla, QtKeychain UI parts
                 │  Subdirectories (internal CMake OBJECT      │     ─ may use libcurl, yaml-cpp, sqlite3,
                 │  libraries, not exposed as separate         │       nlohmann_json, QuickJS, QtKeychain (core)
                 │  CMake targets to the world):               │
                 │   - engine-domain         (no I/O)          │
                 │   - engine-application                      │
                 │   - engine-infrastructure                   │
                 └─────────────────────────────────────────────┘
```

### 2.2 Root `CMakeLists.txt` (outline)

```cmake
cmake_minimum_required(VERSION 3.24)
project(reqloom
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options — let downstream users opt in or out of subsystems
option(REQLOOM_BUILD_DESKTOP "Build the Qt desktop app"     ON)
option(REQLOOM_BUILD_CLI     "Build the CLI binary"         ON)
option(REQLOOM_BUILD_TESTS   "Build the test suite"         ON)
option(REQLOOM_BUILD_IPC     "Build engine IPC server (Phase B)" OFF)
option(REQLOOM_ENABLE_ASAN   "Enable AddressSanitizer in Debug" OFF)

include(cmake/ReqloomCompilerWarnings.cmake)
include(cmake/ReqloomSanitizers.cmake)
include(cmake/ReqloomBoundaryGuards.cmake)   # The firewall enforcement

# Dependencies — managed via Conan or vcpkg
find_package(Qt6 6.6 REQUIRED COMPONENTS Core)   # For all targets
find_package(yaml-cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(qtkeychain REQUIRED)
# QuickJS is vendored under third_party/

# Optional UI-only deps
if(REQLOOM_BUILD_DESKTOP)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Quick QuickWidgets)
    find_package(QScintilla REQUIRED)
endif()

# Subdirs in dependency order
add_subdirectory(engine)
if(REQLOOM_BUILD_CLI)
    add_subdirectory(cli)
endif()
if(REQLOOM_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()
if(REQLOOM_BUILD_IPC)
    add_subdirectory(ipc)
endif()

# Boundary check runs at configure time
reqloom_enforce_boundary_rules()

if(REQLOOM_BUILD_TESTS)
    enable_testing()
endif()
```

### 2.3 `engine/CMakeLists.txt` (outline)

This is where the engine boundary is **mechanically enforced**.

```cmake
# Internal OBJECT libraries — give us layer-level dependency control
# without polluting the public CMake target namespace.

add_library(reqloom-engine-domain OBJECT
    src/domain/DependencyResolver.cpp
    src/domain/Topology.cpp
    src/domain/VariableResolver.cpp
    src/domain/RunContext.cpp
)
target_include_directories(reqloom-engine-domain
    PUBLIC  include
    PRIVATE src
)
# Domain layer: ALLOWED to link only stdlib + QtCore value types.
target_link_libraries(reqloom-engine-domain
    PUBLIC  Qt6::Core         # for QString — replace with std::u8string later if Phase B drops Qt
)

add_library(reqloom-engine-infrastructure OBJECT
    src/infrastructure/http/CurlHttpClient.cpp
    src/infrastructure/storage/SqliteHistoryStore.cpp
    src/infrastructure/secrets/KeychainSecretStore.cpp
    src/infrastructure/schema/YamlSchemaParser.cpp
    src/infrastructure/hooks/QuickJsHookRunner.cpp
    src/infrastructure/extraction/JsonPathEvaluator.cpp
)
target_include_directories(reqloom-engine-infrastructure
    PUBLIC  include
    PRIVATE src
)
target_link_libraries(reqloom-engine-infrastructure
    PUBLIC  reqloom-engine-domain
    PRIVATE
        Qt6::Core
        CURL::libcurl
        yaml-cpp
        SQLite::SQLite3
        nlohmann_json::nlohmann_json
        qtkeychain
        reqloom-quickjs           # vendored target from third_party/
)

add_library(reqloom-engine-application OBJECT
    src/application/RunOperationUseCase.cpp
    src/application/ImportFromOpenApi.cpp
)
target_include_directories(reqloom-engine-application
    PUBLIC  include
    PRIVATE src
)
target_link_libraries(reqloom-engine-application
    PUBLIC  reqloom-engine-domain
    PRIVATE reqloom-engine-infrastructure
)

# The single public engine library — consumed by cli/, desktop/, and (future) ipc/
add_library(reqloom-engine STATIC)        # SHARED also fine; STATIC simplifies linking
target_link_libraries(reqloom-engine
    PUBLIC
        reqloom-engine-domain
        reqloom-engine-application
    PRIVATE
        reqloom-engine-infrastructure
)
target_include_directories(reqloom-engine
    PUBLIC  include
)
add_library(reqloom::engine ALIAS reqloom-engine)

# === BOUNDARY GUARDS — fail the build if forbidden deps creep in ===
reqloom_forbid_dependencies(reqloom-engine
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
    QScintilla
)

# Tests
if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

### 2.4 `cli/CMakeLists.txt` (outline)

```cmake
add_executable(reqloom-cli
    src/main.cpp
    src/commands/RunCommand.cpp
    src/commands/LintCommand.cpp
    src/commands/ImportCommand.cpp
    src/output/TextRenderer.cpp
    src/output/JsonRenderer.cpp
    src/output/JUnitRenderer.cpp
)
target_link_libraries(reqloom-cli
    PRIVATE
        reqloom::engine
        Qt6::Core
)
set_target_properties(reqloom-cli PROPERTIES OUTPUT_NAME "reqloom")

reqloom_forbid_dependencies(reqloom-cli
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

### 2.5 `desktop/CMakeLists.txt` (outline)

```cmake
qt_add_executable(reqloom-desktop WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/application/App.cpp
    src/application/Bootstrapper.cpp
    src/viewmodels/ProjectExplorerViewModel.cpp
    src/viewmodels/RunViewModel.cpp
    src/viewmodels/ResponseViewModel.cpp
    src/viewmodels/EnvironmentViewModel.cpp
    src/views/ProjectExplorerWidget.cpp
    src/views/RequestEditorPanel.cpp
    src/views/ResponseViewerPanel.cpp
    src/views/TimelinePanel.cpp
    src/views/CodeEditor.cpp
    src/views/DependencyGraphView.cpp
    # … the rest
)
qt_add_qml_module(reqloom-desktop
    URI Reqloom.Graph
    VERSION 1.0
    QML_FILES qml/DependencyGraph.qml
)

target_link_libraries(reqloom-desktop
    PRIVATE
        reqloom::engine
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickWidgets
        QScintilla::QScintilla
)

# No boundary guard here — desktop is the one place Qt UI libs are allowed.
```

---

## 3. Where the Boundary Literally Is

The boundary is the **public header surface of `reqloom-engine`** (`engine/include/reqloom/engine/*.h`).

Anything outside the engine directory consumes the engine **only through these headers**. Internal headers (`engine/src/...`) are not on the public include path of the `reqloom-engine` target.

### 3.1 What lives in the public engine surface

```cpp
// engine/include/reqloom/engine/PublicApi.h  (single header for embedders)

#pragma once

#include <reqloom/engine/Operation.h>
#include <reqloom/engine/Actor.h>
#include <reqloom/engine/Resource.h>
#include <reqloom/engine/RunContext.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>
```

```cpp
// engine/include/reqloom/engine/ExecutionEngine.h
#pragma once

#include <reqloom/engine/RunContext.h>
#include <reqloom/engine/Operation.h>
#include <functional>
#include <memory>

namespace reqloom::engine {

class ExecutionEngine {
public:
    struct Dependencies {                         // injected at construction
        std::unique_ptr<HttpClient>   http;
        std::unique_ptr<SchemaParser> schema;
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<SecretStore>  secrets;
        std::unique_ptr<HookRunner>   hooks;
    };

    explicit ExecutionEngine(Dependencies deps);
    ~ExecutionEngine();

    // Synchronous core
    RunResult run(const OperationId& target, RunContext& ctx);

    // Cancellation primitive — language-agnostic
    void cancel(const RunId& run);

    // Observability — engine pushes events; UI/CLI subscribes
    using EventCallback = std::function<void(const RunEvent&)>;
    void subscribe(EventCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;                  // pImpl — ABI-friendly
};

}  // namespace reqloom::engine
```

### 3.2 What stays in the engine but is NOT public

- All concrete implementations (CurlHttpClient, SqliteHistoryStore, QuickJsHookRunner)
- yaml-cpp, libcurl, sqlite3 includes — never appear in public headers
- Internal data structures (Kahn's algorithm scratchpad, JSONPath compilation cache, etc.)

This means a new embedder of `reqloom-engine` (the future Rust binding, the IPC daemon, the CLI test harness) needs only:

```cmake
target_link_libraries(my-thing PRIVATE reqloom::engine)
target_include_directories(my-thing PRIVATE
    $<TARGET_PROPERTY:reqloom::engine,INTERFACE_INCLUDE_DIRECTORIES>
)
```

…and gets no transitive Qt-UI surface, no infra-library headers, nothing leaking.

---

## 4. Boundary Enforcement (`cmake/ReqloomBoundaryGuards.cmake`)

The architectural guardrails in PRD §8.6 must be **mechanically verifiable**, not just review-time guidelines. This module provides two functions:

### 4.1 `reqloom_forbid_dependencies(target lib1 lib2 …)`

Checks at configure time that the target's transitive `INTERFACE_LINK_LIBRARIES` and `LINK_LIBRARIES` do not include any of the forbidden libraries.

```cmake
function(reqloom_forbid_dependencies target)
    set(forbidden ${ARGN})
    get_target_property(direct_deps ${target} LINK_LIBRARIES)
    get_target_property(iface_deps  ${target} INTERFACE_LINK_LIBRARIES)
    set(all_deps ${direct_deps} ${iface_deps})

    foreach(dep IN LISTS all_deps)
        foreach(forbid IN LISTS forbidden)
            if("${dep}" STREQUAL "${forbid}")
                message(FATAL_ERROR
                    "[Reqloom boundary] target '${target}' must not depend "
                    "on '${forbid}'. See PRD §8.6 architectural guardrails."
                )
            endif()
        endforeach()
    endforeach()
endfunction()
```

This catches direct links. Transitive leakage is caught by the runtime check below.

### 4.2 `reqloom_enforce_boundary_rules()`

Runs after all subdirectories are added. Walks the actual link interface of each engine target and asserts forbidden symbols are absent.

```cmake
function(reqloom_enforce_boundary_rules)
    # Engine must not pull in Qt UI even transitively
    set(forbidden_for_engine
        Qt6::Widgets Qt6::Gui Qt6::Quick Qt6::QuickWidgets
        QScintilla::QScintilla
    )

    foreach(target IN ITEMS
        reqloom-engine
        reqloom-engine-domain
        reqloom-engine-application
        reqloom-engine-infrastructure
    )
        if(TARGET ${target})
            reqloom_forbid_dependencies(${target} ${forbidden_for_engine})
        endif()
    endforeach()

    # CLI must not pull in Qt UI either
    if(TARGET reqloom-cli)
        reqloom_forbid_dependencies(reqloom-cli ${forbidden_for_engine})
    endif()
endfunction()
```

### 4.3 CI-time verification — the real teeth

CMake-time checks catch link-time leaks. CI catches **header-include leaks** (where a developer `#include <QWidget>` in an engine `.cpp` even though it doesn't link). A simple grep job in CI:

```yaml
# .github/workflows/boundary-check.yml
- name: Engine boundary check (no Qt UI includes)
  run: |
    if grep -rE '#include\s*<Q(Widget|Window|Application|Quick)' engine/src engine/include; then
      echo "::error::Engine layer must not include Qt UI headers"
      exit 1
    fi
    if grep -rE '#include\s*<Qsci' engine/; then
      echo "::error::Engine layer must not include QScintilla"
      exit 1
    fi
```

A more rigorous version uses `clang-tidy`'s `misc-include-cleaner` plus a bespoke checker, but the grep is a 10-line MVP that catches 95% of regressions.

### 4.4 Architectural unit test

For belt-and-braces, ship a test that loads `reqloom::engine` symbols and asserts none reference Qt-UI:

```cpp
// engine/tests/architecture/NoQtUiSymbolsTest.cpp
TEST(ArchitectureGuardrail, EngineHasNoQtUiSymbols) {
    // After linking against reqloom-engine in CMake, attempting to
    // reference QWidget should be a link error. Validate via nm/objdump:
    // nm libreqloom-engine.a | grep -i 'QWidget\|QApplication' should be empty.
    // Implemented as a CTest add_test(... COMMAND <script>) rather than
    // a runtime check — the script greps the static archive.
}
```

---

## 5. Build & Test Flow

### 5.1 Local development (macOS example)

```bash
# Configure
cmake --preset macos-debug

# Build everything
cmake --build --preset macos-debug

# Run engine unit + integration tests only (fast)
ctest --preset macos-debug --label-regex engine

# Run all tests
ctest --preset macos-debug

# Run desktop app
./build/macos-debug/desktop/reqloom-desktop.app/Contents/MacOS/reqloom-desktop

# Run CLI
./build/macos-debug/cli/reqloom run refund.approve --project samples/marketplace
```

### 5.2 CMakePresets.json (outline)

```json
{
  "version": 4,
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "REQLOOM_BUILD_TESTS": "ON"
      }
    },
    { "name": "macos-debug",   "inherits": "base", "generator": "Ninja", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "REQLOOM_ENABLE_ASAN": "ON" } },
    { "name": "macos-release", "inherits": "base", "generator": "Ninja", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } },
    { "name": "windows-debug", "inherits": "base", "generator": "Ninja", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "linux-debug",   "inherits": "base", "generator": "Ninja", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug",   "REQLOOM_ENABLE_ASAN": "ON" } }
  ]
}
```

### 5.3 CI matrix

| OS | Build type | ASan | Tests | Boundary check |
|---|---|---|---|---|
| macOS 14 (Apple Silicon) | Debug | ✅ | unit + integration | ✅ |
| macOS 14 (Apple Silicon) | Release | — | unit + integration | ✅ |
| Windows Server 2022 | Debug | — | unit + integration | ✅ |
| Windows Server 2022 | Release | — | unit + integration | ✅ |
| Ubuntu 24.04 | Debug | ✅ + UBSan | unit + integration | ✅ |
| Ubuntu 24.04 | Release | — | unit + integration | ✅ |

The boundary-check job runs once per matrix cell. If any check fails the PR is blocked.

---

## 6. The Phase B Path

When the trigger to extract the engine fires (PRD ADR-002), the work is:

1. **Switch `reqloom-engine` from `STATIC` to `SHARED`** — already trivial (one CMake line).
2. **Add a thin IPC façade** under `ipc/` that links `reqloom::engine` and exposes JSON-RPC over stdio (or a Unix socket / named pipe). This is roughly 500–1500 lines of code; the engine itself is unchanged.
3. **Replace the desktop's direct `ExecutionEngine` calls** with an `IpcEngineClient` that talks to the spawned child process. The view-model layer's interface to the engine doesn't change because we already used dependency injection in §2.5's `Bootstrapper.cpp`.
4. **Optionally**, port `engine/src/` to Rust over time — the public C++ headers stay, but the `.cpp` files are replaced with Rust code exposing `extern "C"` equivalents. Because `reqloom::engine` uses pImpl + value types, the public ABI is stable.

None of these steps require touching `desktop/` or `cli/` business logic.

---

## 7. Open Questions

- **Q1**: Use **Conan** or **vcpkg** for dependency management? Conan is more flexible (Conan-Center has yaml-cpp, libcurl, QScintilla packages). vcpkg is simpler. Recommendation: **vcpkg** for solo MVP, switch to Conan if dependency complexity grows.
- **Q2**: Use `Qt6::Core` `QString`/`QByteArray` in engine public API, or stick to `std::string` / `std::span<std::byte>`? Using `QString` means engine still depends on `QtCore` (small, ~10 MB). Using `std` removes Qt entirely from the engine (clean, but every embedder pays UTF-8 conversion cost). **Recommendation**: start with `std::string` everywhere except where `QString` is genuinely the right type (file paths via `QFileInfo`); the engine becomes Qt-free except for file I/O wrappers.
- **Q3**: Do we vendor QuickJS under `third_party/` or take it from a Conan/vcpkg package? **Recommendation**: vendor it — QuickJS is small, single-file, doesn't move fast, and avoids dependency pinning issues.
- **Q4**: Should `reqloom-engine` be header-only-friendly (template-heavy) or compiled (pImpl-heavy)? **Recommendation**: pImpl-heavy. Header-only means rebuilding the world on engine changes; pImpl gives stable ABI and fast incremental builds.

---

**End of Project Layout v1**

> Next steps after this is approved:
> 1. Create the empty repo structure (folders, root `CMakeLists.txt`, `cmake/` modules, `CMakePresets.json`)
> 2. Set up the dependency manager (vcpkg.json or conanfile.txt)
> 3. Write the boundary-check CI job
> 4. Begin Phase 1 of the roadmap (engine + CLI)
