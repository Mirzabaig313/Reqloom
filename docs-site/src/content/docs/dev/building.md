---
title: Building from source
description: "Presets, sanitizers, running the test suite, the boundary guards, and the pre-push hook."
---

Full prerequisites and first-time setup live on the
[installation page](/start/install/#build-from-source). This page covers the parts
you need once you're actually developing.

## Presets

Six, from `CMakePresets.json`:

| Preset | Build type | Sanitizers |
| --- | --- | --- |
| `macos-debug` | Debug | ASan + UBSan |
| `macos-release` | Release | none |
| `linux-debug` | Debug | ASan + UBSan |
| `linux-release` | Release | none |
| `windows-debug` | Debug | none |
| `windows-release` | Release | none |

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
```

Debug presets enable AddressSanitizer and UndefinedBehaviorSanitizer, which makes
binaries noticeably slower but catches memory and UB bugs at the point they
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

852 tests currently pass. A red suite on a clean checkout is a bug — please
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

`clang-tidy` runs per `.clang-tidy`. Warnings are `-Wall -Wextra -Wpedantic
-Wconversion`; `-Werror` is on in CI but not locally, so your build won't break on
a warning while you're mid-change — CI will.

## The pre-push hook

Wire it up once:

```bash
git config core.hooksPath tools/git-hooks
```

It runs `tools/pre-push-check.sh` — configure, build, tests, boundary check —
which is roughly what CI does. Catching a failure locally is much faster than
waiting for a CI round trip.

```bash
./tools/pre-push-check.sh          # run it manually any time
git push --no-verify               # skip it, when you have a reason
```

## Qt notes

Qt comes from [aqtinstall](https://github.com/miurahr/aqtinstall) via
`tools/setup-qt.sh`, **not** vcpkg. Building Qt from source through vcpkg added
45–90 minutes to a cold CI run and blew past job time limits, so the project
doesn't do it.

```bash
./tools/setup-qt.sh
export CMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos"      # or .../gcc_64 on Linux
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

| Platform | Runner |
| --- | --- |
| Linux, Windows | GitHub Actions |
| macOS | Azure DevOps Pipelines |

Both pin the same Qt version and run the same test suite. The docs site deploys
separately from `docs-site/**` changes.

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
