---
title: Contributing
description: "Commit conventions, PR expectations, the test rule that matters most, and where to start if you want a first issue."
---

Reqloom is Apache 2.0 and contributions are welcome. This page is the short
version of what reviewers will look for.

## Before you write code

For anything more than a small fix, **open an issue first**. Two reasons: the
change might already be planned, and the architecture has a hard boundary
([the engine can't depend on Qt UI](/dev/architecture/#the-engine-has-no-ui-dependency))
that's easier to discuss before you've built around it.

Good first contributions:

- Documentation gaps — this site is generated from `docs-site/`
- Closing a partial requirement from the [roadmap](/dev/roadmap/#known-gaps)
- New importer formats, which are self-contained
- Test coverage for an existing behaviour

## Setup

```bash
git clone https://github.com/Mirzabaig313/Reqloom
cd Reqloom
./tools/setup-qt.sh
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --test-dir build/macos-debug --output-on-failure
git config core.hooksPath tools/git-hooks     # pre-push checks
```

Full prerequisites: [building from source](/start/install/#build-from-source).

## Branches and commits

Branch from `main`. Never push directly to `main`.

Conventional commits, subject **≤ 70 characters**:

```
feat(engine): add cookie extraction source
fix(cli): report missing --var value instead of unknown argument
docs(schema): document the !secret tag
test(engine): cover indexed reference out of range
refactor(desktop): extract chain strip into its own component
chore(ci): pin Qt to 6.8.3
perf(engine): avoid copying RunContext in the resolver
ci(release): mark alpha tags as prereleases
```

Prefixes: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`, `ci`.

## The test rule

The one reviewers actually enforce:

:::caution[A test must fail without your change]
Check out the parent commit, run your new test, and confirm it fails. If it
passes there, it isn't exercising the new code. A green test that tests nothing is
worse than no test — it creates false confidence.
:::

Every bug fix lands with a test that reproduces the bug first. Domain-layer code
targets 90%+ coverage; infrastructure coverage isn't measured.

Test conventions:

```cpp
// engine/tests/unit/RunBatchUseCaseTest.cpp
TEST(RunBatchUseCase, fails_with_cycle_when_topology_invalid) {
    // Arrange
    FakeResolver resolver{ResolverFault::Cycle};
    FakeRunner   runner;
    RunBatchUseCase uc{resolver, runner};
    RunContext ctx;

    // Act
    const auto result = uc.execute({{"a"}, {"b"}}, ctx);

    // Assert
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::Cycle);
}
```

- File name mirrors the unit under test
- Test name is `<unit>_<observed_behaviour>` in `snake_case`, describing what *was
  observed*, not what should happen
- Arrange / Act / Assert separated by blank lines
- Use real engine types; fake only I/O
- Each test independent — no shared mutable state
- Use in-memory SQLite and the mock SUT fixtures rather than mocking the database

## Code style

`clang-format` and `clang-tidy` configs are in the repo:

```bash
tools/format.sh
```

The conventions that come up most in review:

- **C++23 only.** No C++26 features — see
  [language rules](/dev/building/#language-rules)
- **`std::expected<T, ReqloomError>`** for new error paths. Add to the existing
  `ErrorCode` enum, don't create a parallel one
- **Brace every control flow block**, even one-liners
- **`std::print` / `std::println`**, never `iostream` chains or `std::endl`
- **No C-style casts**, no `NULL`, no `typedef`, no raw `new`/`delete`
- **`[[nodiscard]]`** on anything returning `expected`, `optional`, or `unique_ptr`
- **`noexcept`** on move constructors, move assignment, and `swap`
- **Files ≤ 800 lines, functions ≤ 50.** Split rather than scroll
- **Close namespaces with a comment** — `}  // namespace reqloom::engine`

Qt-specific, for `desktop/`:

- `Q_OBJECT` in any `QObject` subclass using signals or slots
- Function-pointer `connect()` only — the `SIGNAL()`/`SLOT()` string form is banned
- A lambda passed to `connect()` needs a receiver `QObject*` so it disconnects on
  destruction
- `Q_PROPERTY` setters early-return when the value is unchanged, then emit — or
  QML binding loops

## Comments

Comments explain **why**, not what. If a comment is needed to explain what the
code does, simplify the code.

Required: a one-line purpose at the top of each file; a rationale on any
non-obvious algorithm; a justification on every `NOLINT`; what a magic number
means.

Not wanted: commented-out code, restatements of the obvious, trailing end-of-line
comments, banner separators inside functions, authorship notes, or TODOs without
an issue link. Write `// TODO(#42): handle timeout retry` or don't write one.

## Pull requests

- Open against `main` from a feature branch
- Title concise, under ~70 characters; details go in the description
- Say **what you tested**, not just what you changed
- Failing CI blocks merge
- Don't commit secrets, `build/`, or vcpkg trees — `.gitignore` covers these, but
  check your staging area

If your change adds, completes, or breaks a documented behaviour, say so in the PR
description with the file and the test name that proves it. Maintainers track
requirement status separately, and a claim with no evidence reads as not done.

## Documentation

The docs site is Astro Starlight under `docs-site/`:

```bash
cd docs-site
npm install
npm run dev             # http://localhost:4321
```

Add a page under `src/content/docs/<section>/`, then add it to the sidebar in
`astro.config.mjs`. Frontmatter needs `title` and `description`.

One rule specific to these docs: **verify against the binary, not the design docs.** A
sizeable fraction of the original pages documented intended behaviour that was
never implemented — `--dry-run`, a `secrets:` block, `$.random`. If you document a
flag, run it first.

## Reporting bugs

Include the Reqloom version or commit, your OS, the schema (redacted), the command
you ran, and the output with the `E_*` code. A minimal reproducing schema is worth
more than a description.

Exit code `3` is always a bug — it means an unhandled exception escaped.

## Security

Don't open a public issue for a vulnerability. See
[`SECURITY.md`](https://github.com/Mirzabaig313/Reqloom/blob/main/SECURITY.md).

The two attacker-controlled surfaces are schema parsing and spec import — changes
there get extra scrutiny.

## Licence

Apache 2.0. The engine, CLI, schema, and desktop app are open source. The AI
importer prompt suite and the planned team-workspace sync are designated paid
components; don't add closed-source-only code under the OSS tree without updating
`LICENSE` first.

## Next

- [Architecture](/dev/architecture/) — read this before a structural change
- [Building from source](/dev/building/) — presets, sanitizers, hooks
- [Roadmap](/dev/roadmap/) — the known gaps, if you want one to close
