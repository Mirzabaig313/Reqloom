<div align="center">

# ChainAPI

**A workflow-aware API testing tool that auto-resolves request dependency chains.**

[![Build & Test](https://github.com/Mirzabaig313/ChainAPi/actions/workflows/build.yml/badge.svg)](https://github.com/Mirzabaig313/ChainAPi/actions/workflows/build.yml)
[![Docs](https://img.shields.io/badge/docs-chainapi.github.io-2496ED.svg)](https://chainapi.github.io)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-4.0%2B-064F8C.svg)](https://cmake.org/)
[![Qt](https://img.shields.io/badge/Qt-6.8_LTS-41CD52.svg)](https://www.qt.io/)

</div>

---

ChainAPI treats your API as a graph of resources, actors, and dependencies.
Define each actor (auth flow) and each resource (endpoints + dependencies)
once. Then invoke any endpoint and ChainAPI auto-resolves the entire chain —
login, prerequisites, target call — and executes them in the correct order.

> Postman is an HTTP client. ChainAPI is an API workflow engine.

📖 **Full documentation: [chainapi.github.io](https://chainapi.github.io)**

## How it works

Describe each actor and resource once in YAML. Operations reference the outputs
of other operations (`{{order.order_id}}`) and declare explicit prerequisites
(`depends_on`). From those references the engine builds a dependency graph.

```yaml
# resources/refund.yaml
name: refund

operations:
  request:
    method: POST
    path: /api/v1/orders/{{order.order_id}}/refunds
    actor: customer
    depends_on: [order.pay]
    body: { reason: "Item not as described" }
    extract: { refund_id: $.data.id }

  approve:
    method: POST
    path: /api/v1/admin/refunds/{{refund.refund_id}}/approve
    actor: admin
    depends_on: [refund.request]
```

Invoking `refund.approve` resolves and runs the whole chain in order — actor
logins (reusing cached sessions), `order.create`, `order.pay`, `refund.request`,
then the target — with every extracted ID flowing automatically between steps.

## Why ChainAPI

- **Dependency-aware execution.** Model resources and their prerequisites once;
  ChainAPI computes the execution order and runs the full chain for you.
- **Actors as first-class citizens.** Authentication flows are declared once per
  actor and reused across every request that needs them.
- **Three ways to run.** A pure C++ engine library, a scriptable CLI, and a Qt 6
  desktop app — all driven by the same project definition.
- **Clean architecture, enforced.** The engine has zero UI dependency, and that
  boundary is mechanically verified at configure time and in CI.
- **Reproducible runs.** Every step is recorded, secrets are masked, and results
  export to text, JSON, and JUnit for pipelines.

## Features

### Current capabilities

- **Dependency-graph execution** — resources, actors, and operations form a
  directed graph resolved with a topological sort; the engine runs prerequisites
  in order and stops on first failure.
- **Named auth strategies** — `simple`, `chain` (multi-step / OTP), `basic`,
  `api_key`, `oauth2_client_credentials`, and `oauth2_password` with per-actor
  session caching and TTL-based refresh.
- **Variable resolution** — reference session and resource outputs
  (`{{order.order_id}}`), environment values, and built-ins (`{{$.uuid}}`,
  `{{$.now+5m}}`, base64/hex/url codecs).
- **Polling** — model async `202 Accepted` flows with `poll_until`
  (predicate-driven success/fail, fixed interval or exponential backoff, and
  wall-clock plus attempt-count caps).
- **Scripting hooks** — sandboxed pre-request / post-response JavaScript
  (QuickJS) for custom signing and payload transforms, with crypto helpers
  pre-bound on the hook context.
- **Variable extraction** — JSONPath, header, status-code, cookie, and regex
  sources.
- **Request bodies** — JSON, form-data, `x-www-form-urlencoded`, multipart file
  uploads, and raw payloads.
- **Secret references** — `{{secret.NAME}}` values resolved on demand from the
  OS keychain and masked in events, logs, and on-disk history.
- **CLI reporters** — text, JSON, and JUnit output with CI-friendly exit codes.
- **Cross-platform** — macOS, Linux, and Windows.

### Planned

Committed directions from the [product roadmap](https://chainapi.github.io). The
guiding rule is that the **same `chainapi.yaml`** that powers testing also powers
these features — no parallel definitions.

- **Desktop UI** *(in progress)* — project explorer, request editor, response
  viewer, and a run timeline, with a visual dependency graph.
- **Collection import** — direct import of Postman, Bruno, and Insomnia
  collections for zero-friction migration.
- **Mock server mode** — flip a project into "serve" mode to expose a local HTTP
  server returning recorded or templated responses for its operations.
- **Schema-driven documentation** — render a project to a browsable docs site
  with example request/response pairs captured from real runs.
- **Team sync** — git-based sharing of projects and run state.
- **Editor integrations** — VS Code extension and a browser request-capture
  helper.
- **Streaming protocols** — WebSocket, SSE, and GraphQL subscription support.

## Repository layout

```
chainapi/
├── cmake/        Reusable CMake modules (warnings, sanitizers, boundary guards)
├── engine/       libchainapi-engine — pure C++ engine (no Qt UI deps)
│   ├── include/  Public engine API (chainapi/engine/*.h)
│   ├── src/      domain / application / infrastructure layers
│   └── tests/    Unit + integration tests (GoogleTest)
├── cli/          chainapi CLI — links engine + Qt6::Core only
├── desktop/      Qt 6 desktop UI app
├── ipc/          Engine-as-separate-process scaffold (opt-in, off by default)
├── third_party/  Vendored dependencies
├── samples/      Bundled sample projects
└── tools/        Format, lint, and CI helpers
```

## Architecture

The engine is a pure C++ library with **no Qt UI dependency**. The boundary is
mechanically enforced in three places:

1. **CMake link guards** (`cmake/ChainApiBoundaryGuards.cmake`) — fail the
   configure step if the engine or CLI transitively links Qt Widgets / Gui /
   Quick / QScintilla.
2. **CI grep job** (`tools/ci/boundary-check.sh`) — catches `#include`
   regressions before they land.
3. **Public header surface** (`engine/include/chainapi/engine/`) — pImpl and
   value types only; no Qt UI types appear.

This keeps the engine portable and embeddable: extracting it into a separate
process becomes a build-system change rather than a rewrite.

## Getting started

### Prerequisites

- **CMake** 4.0+
- **C++23** compiler (Apple Clang 16+ / Clang 18+ / GCC 14+ / MSVC 19.40+)
- **Qt 6.8 LTS** (installed via `tools/setup-qt.sh`)
- **clang-format 18** (`brew install llvm@18` on macOS, `apt install clang-format-18` on Linux)
- **vcpkg** for the non-Qt dependencies (curl, sqlite3, yaml-cpp, nlohmann-json, gtest)

### First-time setup

Qt is installed out-of-band via [`aqtinstall`](https://github.com/miurahr/aqtinstall)
rather than vcpkg, because building qtbase from source took 45-90 minutes per
cold-cache CI run. The helper script downloads pre-built Qt from the official Qt
mirror — the same artifacts the Qt online installer uses.

```bash
# 1. System build prerequisites (macOS)
brew install ninja autoconf autoconf-archive automake libtool pkg-config llvm@18

# 2. vcpkg (one-time)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc

# 3. Qt 6.8 LTS via aqtinstall
./tools/setup-qt.sh
echo 'export CMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos"' >> ~/.zshrc
echo 'export PATH="/opt/homebrew/opt/llvm@18/bin:$PATH"' >> ~/.zshrc
exec zsh

# 4. Enable the project's git hooks
git config core.hooksPath tools/git-hooks
```

On Linux, substitute `apt-get install ...` and the `gcc_64` Qt subdir
(`$HOME/Qt/6.8.3/gcc_64`). On Windows, use `tools\setup-qt.cmd` and
`C:\Qt\6.8.3\msvc2022_64`.

### Build and run

```bash
# macOS Debug (with ASan + UBSan)
cmake --preset macos-debug
cmake --build --preset macos-debug

# Run the test suite
ctest --preset macos-debug

# Run the CLI
./build/macos-debug/cli/chainapi --help

# Run the desktop app
./build/macos-debug/desktop/ChainAPI.app/Contents/MacOS/ChainAPI
```

Other presets: `macos-release`, `linux-debug`, `linux-release`,
`windows-debug`, `windows-release` — all defined in `CMakePresets.json`.

### Try a sample

The repository bundles a `marketplace` sample project. Run any operation and the
engine resolves and executes its full dependency chain — logging in the required
actors, creating prerequisites, then calling the target:

```bash
# Execute the chain ending at refund.approve
./build/macos-debug/cli/chainapi run refund.approve --project samples/marketplace

# Validate the schema
./build/macos-debug/cli/chainapi lint --project samples/marketplace

# Emit JUnit XML for CI
./build/macos-debug/cli/chainapi run refund.approve \
  --project samples/marketplace --format junit --output results.xml
```

### CLI reference

```
chainapi run <operation> [opts]   Execute a chain ending at <operation>
  --project <path>                Project directory (default: cwd)
  --env <name>                    Environment to run against
  --var KEY=VALUE                 Override an env variable (repeatable)
  --format text|json|junit        Output format (default: text)
  --output <file>                 Write rendered output to <file>
  --quiet                         Suppress live progress on stdout
chainapi lint                     Validate the schema in the current project
chainapi import <file>            Import an external API spec
chainapi --help                   Show usage
```

### Build options

| Option                       | Default | Effect                                  |
|------------------------------|---------|-----------------------------------------|
| `CHAINAPI_BUILD_DESKTOP`     | ON      | Build the Qt desktop app                |
| `CHAINAPI_BUILD_CLI`         | ON      | Build the CLI binary                    |
| `CHAINAPI_BUILD_TESTS`       | ON      | Build the test suite                    |
| `CHAINAPI_BUILD_IPC`         | OFF     | Build the IPC server                    |
| `CHAINAPI_ENABLE_ASAN`       | OFF     | Enable AddressSanitizer in Debug        |
| `CHAINAPI_ENABLE_UBSAN`      | OFF     | Enable UBSan in Debug                   |

## Secret storage

Projects reference credentials with `{{secret.NAME}}` in any template (headers,
body, auth config, poll predicates). At run start the engine reads exactly the
referenced names from the OS keychain into the run — it never bulk-dumps the
keychain — and substitutes them into outbound requests. Values are masked in
events, logs, and on-disk history.

The backend is [QtKeychain](https://github.com/frankosterfeld/qtkeychain)
(macOS Keychain, Windows Credential Store, libsecret/KWallet on Linux). It is
**not** part of Qt and is not shipped by aqtinstall, so it resolves one of two
ways:

1. **System install (used as-is if present).** The top-level
   `find_package(Qt6Keychain CONFIG QUIET)` picks it up:

   ```bash
   # macOS
   brew install qtkeychain
   # Ubuntu / Debian
   sudo apt-get install -y qtkeychain-qt6-dev
   ```

2. **Built from source via FetchContent (the fallback).** When no system
   QtKeychain is found, `third_party/CMakeLists.txt` fetches and builds it
   (v0.14.0, ~20-30s) against the already-present Qt6. This is what CI uses.

If QtKeychain is absent entirely, the engine falls back to a no-op store and
`{{secret.X}}` references resolve to empty — useful for CI smoke builds, but not
a production path.

> **Note:** the first keychain access from a freshly built (unsigned) binary
> triggers the OS authorization prompt — launch the app or CLI normally and
> allow it once.

## Development

Run the full pre-push smoke check (format + configure + build + tests + boundary)
before pushing:

```bash
./tools/pre-push-check.sh
```

If you ran `git config core.hooksPath tools/git-hooks`, this also runs
automatically on `git push`. Bypass when justified with `git push --no-verify`.

CI fails the whole pipeline on a single misformatted line, using
**clang-format 18** specifically. Mirror that check locally:

```bash
./tools/format.sh
```

## Documentation

Full documentation — schema reference, auth strategies, polling, hooks, and the
CLI guide — lives at **[chainapi.github.io](https://chainapi.github.io)**.

The site is built from `docs-site/` (Astro Starlight). To run it locally:

```bash
cd docs-site
npm install
npm run dev   # → http://localhost:4321
```

Pushes to `main` that touch `docs-site/` auto-deploy via
`.github/workflows/deploy-docs.yml`.

## Project status

The C++ engine and the `chainapi` CLI are functional: the engine resolves and
executes dependency chains, named auth strategies, polling, hooks, and secret
substitution, covered by an extensive unit and integration test suite. The Qt 6
desktop app (project explorer, request editor, response viewer, run timeline) is
under active development.

## Contributing

Contributions are welcome. Please:

1. Fork the repository and branch from `main`.
2. Follow the existing code style (`clang-format` / `clang-tidy` configs are in
   the repo root) and run `./tools/pre-push-check.sh` before opening a PR.
3. Use [Conventional Commits](https://www.conventionalcommits.org/) for commit
   messages (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `chore:`).
4. Open a pull request; passing CI is required to merge.

Report security issues privately as described in [`SECURITY.md`](SECURITY.md).

## License

Apache License 2.0 — see [`LICENSE`](LICENSE). The open-source license covers the
engine, CLI, schema spec, and Qt desktop app.
