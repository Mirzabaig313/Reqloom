---
title: Building from source
description: "Presets, sanitizers, running the test suite, the boundary guards, and the pre-push hook."
---

Full prerequisites and first-time setup live on the
[installation page](/start/install/#build-from-source). This page covers the parts
you need once you're actually developing.

## Presets

Six, from `CMakePresets.json`:

| Preset | Build type | Sanitizers | Qt |
| --- | --- | --- | --- |
| `macos-debug` | Debug | none | Homebrew `qt@6` at `/opt/homebrew/opt/qt@6` |
| `macos-release` | Release | none | Homebrew `qt@6` |
| `linux-debug` | Debug | ASan + UBSan | `CMAKE_PREFIX_PATH` |
| `linux-release` | Release | none | `CMAKE_PREFIX_PATH` |
| `windows-debug` | Debug | none | `CMAKE_PREFIX_PATH` |
| `windows-release` | Release | none | `CMAKE_PREFIX_PATH` |

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
```

Every preset takes the vcpkg toolchain from `$VCPKG_ROOT`, so that variable must
be set before you configure; without it, configure fails with *"Could not find
toolchain file"*.

Only `linux-debug` enables AddressSanitizer and UndefinedBehaviorSanitizer, which
makes binaries noticeably slower but catches memory and UB bugs at the point they
happen. Develop on debug; benchmark on release.

## Running tests

```bash
# Everything
ctest --test-dir build/macos-debug --output-on-failure

# In parallel
ctest --test-dir build/macos-debug -j $(sysctl -n hw.ncpu) --output-on-failure

# Engine only — fastest feedback loop
ctest --test-dir build/macos-debug --label-regex engine

# One test by name
ctest --test-dir build/macos-debug -R DependencyResolver
```

The suite registers 1,129 tests. A red suite on a clean checkout is a bug — please
[report it](https://github.com/Mirzabaig313/Reqloom/issues) with your platform and
compiler version.

## Layout of the build

```
build/macos-debug/
├── cli/reqloom                       the CLI
├── desktop/Reqloom.app               the desktop app (macOS)
├── engine/                           static library + object libraries
└── vcpkg_installed/                  dependencies
```

```bash
./build/macos-debug/cli/reqloom --help
./build/macos-debug/desktop/Reqloom.app/Contents/MacOS/Reqloom
```

## Boundary guards

The engine must not acquire a Qt UI dependency. Two guards will stop you, and
it's worth knowing which one you've hit:

**At configure time.** `cmake/ReqloomBoundaryGuards.cmake` fails if the engine,
CLI, or engine tests link `Qt6::Widgets`, `Qt6::Gui`, `Qt6::Quick`,
`Qt6::QuickWidgets`, or `QScintilla`. You'll see it as a CMake error before any
compilation.

**In CI.** A grep job rejects `#include <QWidget>`, `<QApplication>`, or
`<Qsci…>` under `engine/` or `cli/`.

If you need to surface engine state in the UI, do it through
`engine/include/reqloom/engine/Events.h` callbacks or by extending `PublicApi.h`.
Don't add a shared target that imports both worlds — see
[architecture](/dev/architecture/#the-engine-has-no-ui-dependency).

When adding a new engine sub-target, add it to the
`reqloom_forbid_dependencies(...)` loop in `engine/CMakeLists.txt` so the new
target is covered too.

## Adding a source file

Source lists are explicit — there is no `file(GLOB ...)` anywhere, deliberately,
so a new file can't silently fail to build.

1. Add the path to the right `add_library(reqloom-engine-<layer> OBJECT ...)`
   block in `engine/CMakeLists.txt`
2. Add a unit test under `engine/tests/unit/`
3. If it pulls in a new third-party, add it to `vcpkg.json`

## Formatting and linting

```bash
tools/format.sh            # clang-format over the tree
```

Formatting is pinned to **clang-format 18**. The lightest install is
`pipx install clang-format==18.1.8`; `brew install llvm@18` (macOS) and
`apt install clang-format-18` (Linux) also work. The pre-push hook checks
formatting; CI doesn't, so the hook is where an unformatted change is caught.

`clang-tidy` runs per `.clang-tidy`. Warnings include `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow` (`/W4` on MSVC). Warnings are not errors, locally or in
CI.

## The pre-push hook

Wire it up once:

```bash
git config core.hooksPath tools/git-hooks
```

It runs `tools/pre-push-check.sh` — clang-format check, configure, build, tests,
boundary check — which is roughly what CI does, plus the format check.
`SKIP_FORMAT=1` skips the format step when you have to. Catching a failure locally is much faster than
waiting for a CI round trip.

```bash
./tools/pre-push-check.sh          # run it manually any time
git push --no-verify               # skip it, when you have a reason
```

## Qt notes

Qt never comes from vcpkg. Building Qt from source through vcpkg added 45–90
minutes to a cold CI run and blew past job time limits, so the project doesn't
do it.

- **macOS, locally:** Homebrew's `qt@6`, which the `macos-*` presets find at
  `/opt/homebrew/opt/qt@6`. To use another Qt, pass
  `-DCMAKE_PREFIX_PATH=<path>` when configuring.
- **Linux and Windows, and CI on every platform:** pre-built Qt 6.8.3 from the
  official mirror via [aqtinstall](https://github.com/miurahr/aqtinstall).

```bash
./tools/setup-qt.sh                                   # Linux
export CMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/gcc_64"
```

`QT_VERSION` is pinned in `tools/setup-qt.sh` and in the CI workflows — keep them
in sync if you bump it.

## Language rules

**C++23 only.** C++26 features are not portable across the three CI compilers and
are rejected in review: reflection (`^^T`), contracts (`pre`/`post`),
`std::execution`, `std::inplace_vector`, `std::hive`, `= delete("reason")`.

Use C++23 freely — `std::expected`, `std::print`/`std::println`, `deducing this`,
and the ranges additions (`views::zip`, `views::enumerate`, `ranges::to`) are all
stable on Apple Clang 16, Clang 18, GCC 14, and MSVC 19.40.

New error paths return `std::expected<T, ReqloomError>`. Add a code to the
existing `ErrorCode` enum rather than introducing a parallel error type — the
enum is the QA contract.

## CI

All three platforms build on **GitHub Actions** (`ubuntu-latest`,
`windows-latest`, `macos-latest`) in one workflow, after a static boundary check.
A push to `dev` builds the debug presets and runs the tests; a push to `main`
builds the release presets. Every platform pins the same Qt version.

Pushes that only touch docs (`docs-site/**`, `doc/**`, Markdown files) skip the
platform builds; the docs site builds and deploys from its own workflow.

## Troubleshooting

Build problems are covered on the
[installation page](/start/install/#troubleshooting) — missing CMake 4.0, Qt not
found, stale vcpkg caches, and missing shared libraries.

Two more that only show up in development:

**Sanitizer failure in an unrelated test.** ASan reports where the corruption was
*detected*, not always where it originated. Run the single failing test in
isolation with `ctest -R <name> --output-on-failure` before assuming the cause.

**Configure fails with a boundary error.** You've linked a Qt UI target into the
engine or CLI. Read the CMake error — it names the target and the forbidden
dependency.

## Next

- [Architecture](/dev/architecture/) — layering and the run flow
- [Contributing](/dev/contributing/) — commits, PRs, review
