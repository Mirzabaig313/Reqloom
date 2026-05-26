# ChainAPI

> **A workflow-aware API testing tool that auto-resolves request dependency chains.**

📖 **Documentation: [chainapi.github.io](https://chainapi.github.io/)** (or the project-pages URL after first deploy)

ChainAPI treats your API as a graph of resources, actors, and dependencies.
Define each actor (auth flow) and each resource (endpoints + dependencies)
once. Then click any endpoint and ChainAPI auto-resolves the entire chain —
login, prerequisites, target call — and executes them in the correct order.

> Postman is an HTTP client. ChainAPI is an API workflow engine.

See [`doc/ChainAPI - PRD.md`](doc/ChainAPI%20-%20PRD.md) for the full product spec.

---

## Repository Layout

```
chainapi/
├── doc/                    Product documentation (PRD, engine spec, layout)
├── cmake/                  Reusable CMake modules (warnings, sanitizers, boundary guards)
├── engine/                 libchainapi-engine — pure C++ engine (no Qt UI deps)
│   ├── include/            Public engine API
│   ├── src/                Domain / application / infrastructure layers
│   └── tests/              Unit + integration tests
├── cli/                    chainapi CLI — links engine + Qt6::Core only
├── desktop/                Qt 6 desktop UI app
├── ipc/                    Phase B scaffold (engine-as-separate-process)
├── third_party/            Vendored deps (QuickJS lands here in Phase 1)
├── samples/marketplace/    Bundled MarketplaceAPI sample project
├── prompts/import/         AI importer prompt templates (Phase 3)
└── tools/                  Format, lint, CI helpers
```

The architecture is documented in [`doc/ChainAPI - Project Layout.md`](doc/ChainAPI%20-%20Project%20Layout.md).

---

## Architectural Boundary (Why It Matters)

The engine is a pure C++ library with **no Qt UI dependency**. The
boundary is mechanically enforced in three places:

1. **CMake link guards** (`cmake/ChainApiBoundaryGuards.cmake`) — fail
   the configure step if the engine or CLI transitively links Qt
   Widgets / Gui / Quick / QScintilla.
2. **CI grep job** (`tools/ci/boundary-check.sh`, run by both
   `appveyor.yml` and the static-checks stage of `azure-pipelines.yml`) —
   catches `#include` regressions before they land.
3. **Public header surface** (`engine/include/chainapi/engine/`) — pImpl
   + value types only, no Qt UI types appear.

This keeps the future Phase B option open: extracting the engine into a
separate process or a rewrite becomes a build-system change rather
than a rewrite. See PRD §8.6 / ADR-002.

---

## Building

### Prerequisites

- **CMake** 4.0+
- **C++23** compiler (Apple Clang 16+ / Clang 18+ / GCC 14+ / MSVC 19.40+)
- **Qt 6.8 LTS** (installed via `tools/setup-qt.sh` — see below)
- **clang-format 18** (matches CI; from `brew install llvm@18` on macOS or `apt install clang-format-18` on Linux)
- **vcpkg** for the non-Qt deps (curl, sqlite3, yaml-cpp, nlohmann-json, gtest)

### First-time setup

Qt is installed out-of-band via [`aqtinstall`](https://github.com/miurahr/aqtinstall),
not vcpkg, because building qtbase from source took 45-90 minutes per
cold-cache CI run and exceeded AppVeyor's 60-minute job cap. The
helper script downloads pre-built Qt from the official Qt mirror —
same artifacts the Qt online installer uses:

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

On Linux substitute `apt-get install ...` and the `gcc_64` Qt subdir
(`$HOME/Qt/6.8.3/gcc_64`). On Windows use `tools\setup-qt.cmd` and
`C:\Qt\6.8.3\msvc2022_64`.

### Configure & build

```bash
# macOS Debug (with ASan + UBSan)
cmake --preset macos-debug
cmake --build --preset macos-debug

# Run engine tests
ctest --preset macos-debug

# Run the CLI
./build/macos-debug/cli/chainapi --help

# Run the desktop app
./build/macos-debug/desktop/ChainAPI.app/Contents/MacOS/ChainAPI
```

Other presets: `macos-release`, `linux-debug`, `linux-release`,
`windows-debug`, `windows-release` — all defined in `CMakePresets.json`.

### Before pushing

Run the full pre-push smoke check (format + configure + build + tests + boundary):

```bash
./tools/pre-push-check.sh
```

If you ran `git config core.hooksPath tools/git-hooks` above, this also
runs automatically on `git push`. Bypass when justified with
`git push --no-verify`.

#### Just the format check

CI fails the whole pipeline on a single misformatted line. Mirror that
check locally before pushing. CI uses **clang-format 18** specifically —
match that version or the local check may pass while CI fails.

```bash
# Check (read-only — exits non-zero on drift). Pin to v18 explicitly so
# a newer clang-format on PATH doesn't disagree with CI.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format-18}"
find engine cli desktop ipc \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) \
    -not -path '*/build/*' \
    -print0 \
  | xargs -0 "$CLANG_FORMAT" --dry-run --Werror

# Fix (rewrites files in place; uses the same v18 binary)
./tools/format.sh
```

If `clang-format-18` isn't on PATH, install it:

```bash
# macOS
brew install llvm@18
export PATH="/opt/homebrew/opt/llvm@18/bin:$PATH"

# Ubuntu / Debian
sudo apt-get install -y clang-format-18
```

The first command is exactly what runs in `appveyor.yml` and
`azure-pipelines.yml`. If it exits 0, CI's static-checks job will pass.

### Build options

| Option                       | Default | Effect                                      |
|------------------------------|---------|---------------------------------------------|
| `CHAINAPI_BUILD_DESKTOP`     | ON      | Build the Qt desktop app                    |
| `CHAINAPI_BUILD_CLI`         | ON      | Build the CLI binary                        |
| `CHAINAPI_BUILD_TESTS`       | ON      | Build the test suite                        |
| `CHAINAPI_BUILD_IPC`         | OFF     | Build the Phase B IPC server (post-MVP)     |
| `CHAINAPI_ENABLE_ASAN`       | OFF     | Enable AddressSanitizer in Debug            |
| `CHAINAPI_ENABLE_UBSAN`      | OFF     | Enable UBSan in Debug                       |

---

## Status

Phase 0 (validation) and Phase 1 (engine + CLI) complete. The engine runs
the marketplace sample and the GiGwala backend (174 endpoints, 5 actors,
29 resources) end-to-end with auto-resolved dependency chains. 188 tests
green across unit + integration suites.

Next up: **Phase 2** (desktop UI) per PRD §13.3.

---

## Documentation index

For the AI importer:

- **[`prompts/import/README.md`](prompts/import/README.md)** — multi-stage prompt suite overview
- **[`prompts/import/01-discover.md`](prompts/import/01-discover.md)** through **`06-fix-lint-errors.md`** — individual stage prompts

For working with the bundled samples:

- **[`samples/marketplace/`](samples/marketplace/)** — 30-endpoint marketplace sample project. Try `chainapi run refund.approve --project samples/marketplace`.

## Documentation site

The user-facing docs live at **`docs-site/`** as an Astro Starlight
project. Build and run locally:

```bash
cd docs-site
npm install
npm run dev   # → http://localhost:4321
```

Pushes to `main` that touch `docs-site/`, `doc/`, or `prompts/import/`
auto-deploy to GitHub Pages via `.github/workflows/deploy-docs.yml`.
The workflow auto-detects org-page vs project-page repo type, so it
works for both `chainapi.github.io` and any `<owner>.github.io/<repo>/`
URL.

---

## License

Apache License 2.0 — see [`LICENSE`](LICENSE).

The open-source license covers the engine, CLI, schema spec, and Qt
desktop app. Per PRD §15, the AI importer and team-workspace cloud sync
are planned as paid features in a separate, closed component.
