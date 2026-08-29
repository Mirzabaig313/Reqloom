---
title: CLI overview
description: "The three reqloom subcommands — run, lint, import — with their exit codes and the flags each one actually accepts."
---

`reqloom` has three subcommands and no global configuration file. Everything is
a flag or a project directory.

:::tip[The CLI is one of two front ends]
The [desktop app](/desktop/overview/) drives the same engine and the same YAML.
Use the app to build and debug a schema — it has a step timeline, response
inspection, and the only way to store a keychain secret. Use the CLI to run one, in
CI or from a script. Neither is a subset of the other; see
[which to use](/desktop/overview/#which-one-to-use).
:::

| Command | Does | Typical use |
| --- | --- | --- |
| [`reqloom run`](/cli/run/) | Resolves and executes a chain ending at one operation | Local development, CI |
| [`reqloom lint`](/cli/lint/) | Validates a schema and dry-runs every operation | Pre-commit hooks, CI gate |
| [`reqloom import`](/cli/import/) | Converts an existing API definition into a project | One-time migration |

```bash
reqloom --help
```

## Flags are per-command

There is no shared flag set beyond `--project`. Each command parses its own
arguments, and passing a flag to the wrong command either errors or is ignored:

| Flag | `run` | `lint` | `import` |
| --- | --- | --- | --- |
| `--project <path>` | yes | yes | — |
| `--env <name>` | yes | — | — |
| `--var KEY=VALUE` | yes | — | — |
| `--format <fmt>` | yes | — | — |
| `--output <file>` | yes | — | — |
| `--quiet` | yes | — | — |
| `--out <dir>` | — | — | yes |
| `--project-root <dir>` | — | — | yes |
| `--force` | — | — | yes |
| `--help` / `-h` | yes | — | yes |

Two things to know about that table:

- **`lint` ignores unknown arguments silently.** `reqloom lint --format json`
  does not error — it runs a normal lint and discards the flag. There is no
  `lint --help` either; use `reqloom --help`.
- **`run` and `import` require the positional argument first.**
  `reqloom run --env staging order.create` fails, because the first argument is
  always read as the operation id. Write `reqloom run order.create --env staging`.

## Where the project comes from

Every command works against a **project directory** — a folder containing a
`reqloom.yaml`. `run` and `lint` default to the current directory:

```bash
cd my-api && reqloom lint              # uses ./reqloom.yaml
reqloom lint --project ../my-api       # or point at it explicitly
```

If there's no `reqloom.yaml` there, you get an error and exit code 1:

```ansi
Error: reqloom.yaml not found in /Users/you/somewhere
```

## Exit codes

The same four codes across all three commands. They're designed so CI can tell
a genuine test failure apart from a broken invocation.

| Code | Meaning |
| --- | --- |
| `0` | Success. The chain passed, the schema is clean, or the import was written |
| `1` | Ran correctly, result was bad — failing step, schema error, or write failure |
| `2` | You invoked it wrong — unknown flag, missing operation, bad `--format` |
| `3` | Reqloom crashed. Please [report it](https://github.com/Mirzabaig313/Reqloom/issues) |

In a CI pipeline, treat `1` as "the API is broken" and `2` or `3` as "the
pipeline is broken":

```bash
reqloom run checkout.complete --format junit --output results.xml
case $? in
  0) echo "pass" ;;
  1) echo "API failure — see results.xml" ;;
  *) echo "reqloom invocation or crash — check the command" ; exit 1 ;;
esac
```

The exit code does **not** encode which error occurred. Every schema, network,
and assertion failure is exit `1`; the specific `E_*` code appears in the
output. See [error codes](/reference/error-codes/) for the full list.

## stdout vs stderr

Deliberately split so you can pipe one without the other:

- **stdout** — the rendered result: the summary table, or the JSON/JUnit
  document, or `import`'s success line.
- **stderr** — failures and diagnostics: failed steps, schema errors, and
  `import`'s review notes.

Failed steps print to stderr **even under `--quiet`**, so a CI log always shows
what broke:

```bash
reqloom run order.pay --quiet > /dev/null    # stderr still reports failures
```

## Next

- [`reqloom run`](/cli/run/) — output formats, variable overrides, CI recipes
- [`reqloom lint`](/cli/lint/) — what it checks, and what it can't
- [`reqloom import`](/cli/import/) — the seven formats it accepts
