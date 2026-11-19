---
title: CLI overview
description: "The reqloom subcommands — run, lint, list, describe, plan, show, hosts, import — with their exit codes, the flags each one accepts, and the shared JSON contract."
---

`reqloom` has no global configuration file. Everything is a flag or a project
directory.

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
| [`reqloom list`](/cli/list/) | Every actor, resource and operation, with method and path | Finding your way around a project |
| [`reqloom describe`](/cli/describe/) | One operation: what it consumes and from where, what it extracts, who needs it | Before editing an operation |
| [`reqloom plan`](/cli/plan/) | The chain `run` would execute, its sessions, secrets and hosts, without sending | Before a run; "what must exist first?" |
| [`reqloom show`](/cli/show/) | What one step of an earlier run sent and received, masked | Diagnosing a failed run |
| [`reqloom hosts`](/cli/run/#reqloom-hosts) | Lists, approves and revokes the hosts that may receive credentials | Once per project and host |
| [`reqloom import`](/cli/import/) | Converts an existing API definition into a project | One-time migration |

`list`, `describe`, `plan` and `show` only read: they send nothing, read no
secret value and write no project file. Authoring is editing the YAML and
checking it with `reqloom lint`; there is no command that writes project files
other than `import`.

[Install it](/start/install/#install-the-cli) with one command; it is released
with the desktop app and always carries the same version.

```bash
curl -fsSL https://reqloom.tech/install.sh | sh
reqloom --help
```

## Flags are per-command

There is no shared flag set beyond `--project`. Each command has its own
options, and an option a command doesn't know is exit code `2`:

| Flag | `run` | `lint` | `list` | `describe`, `plan` | `show` | `import` |
| --- | --- | --- | --- | --- | --- | --- |
| `--project <path>` | yes | yes | yes | yes | yes | — |
| `--env <name>` | yes | — | — | yes | — | — |
| `--var KEY=VALUE` | yes | — | — | — | — | — |
| `--format <fmt>` | `text`, `json`, `junit` | `text`, `json` | `text`, `json` | `text`, `json` | `text`, `json` | — |
| `--output <file>` | yes | — | — | — | — | — |
| `--quiet` | yes | — | — | — | — | — |
| `--failure-body-size <size>` | yes | — | — | — | — | — |
| `--response-body-size <size>` | yes | — | — | — | — | — |
| `--non-interactive` | yes | — | — | — | — | — |
| `--agent <name>` | yes | — | — | — | — | — |
| `--path <jsonpath>`, `--size <size>` | — | — | — | — | yes | — |
| `--out <dir>` | — | — | — | — | — | yes |
| `--project-root <dir>` | — | — | — | — | — | yes |
| `--force` | — | — | — | — | — | yes |
| `--no-secrets` | — | — | — | — | — | yes |
| `--help` / `-h` | yes | yes | yes | yes | yes | yes |

Options may come before or after the positional argument, and `--name=value`
works as well as `--name value`. `reqloom --version` prints the version.

## Where the project comes from

Every command works against a **project directory** — a folder containing a
`reqloom.yaml`. `run` and `lint` default to the current directory:

```bash
cd my-api && reqloom lint              # uses ./reqloom.yaml
reqloom lint --project ../my-api       # or point at it explicitly
```

If there's no `reqloom.yaml` there, you get an error and exit code `2`:

```ansi
Error: reqloom.yaml not found in /Users/you/somewhere
```

## Exit codes

The same codes across every command. They're designed so CI can tell a
genuine test failure apart from a broken invocation.

| Code | Meaning |
| --- | --- |
| `0` | Success. The chain passed, the schema is clean, or the import was written |
| `1` | Ran correctly, result was bad — a step or assertion failed, the project doesn't validate, the engine refused the run, or the import target already holds a project |
| `2` | You invoked it wrong — unknown option, missing or invalid value, no `reqloom.yaml` at the project path, unknown operation or unknown `--env` |
| `3` | File or system error — the report or the imported project couldn't be written, or an unexpected error. For the last, please [report it](https://github.com/Mirzabaig313/Reqloom/issues) |
| `130` / `143` | `run` was interrupted by SIGINT (Ctrl-C) or SIGTERM. The run is cancelled and its report is still written |

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
output. With `--format json` or `junit`, every outcome writes exactly one
document, including an error before the first step. See [error codes](/reference/error-codes/) for the full list.

## The JSON contract

Every command's `--format json` document starts with the same fields and follows
the same rules, so an agent or a script can rely on them:

- `schema_version` is `1`. It changes only if a field is renamed or removed,
  which does not happen within a major version.
- `contract_version` is `<schema_version>.<minor>`; the minor number grows with
  each set of added fields. `1.1` added the response and `next_action` to
  `run`; `1.2` added `list`, `describe`, `plan` and `show`, positions and fixes
  on `lint` errors, and `recorded` on `run`.
- `list`, `describe`, `plan` and `show` also carry `command` and `ok`.
- Content from the API under test sits in its own field marked
  `"source": "system_under_test", "untrusted": true`; it is never Reqloom's
  instructions.
- No document contains a secret, token, cookie or `Authorization` value.

### JSON errors

When a command cannot answer, its document has `ok: false` and an `error`:

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "describe",
  "ok": false,
  "error": {
    "code": "E_CLI_UNKNOWN_OPERATION",
    "detail": "unknown operation 'order.nope'; the project has: order.create, order.pay",
    "next_action": "Name one of the operations listed in error.detail, as <resource>.<operation>.",
    "file": null,
    "line": null
  }
}
```

`code` is an engine code from the [error codes](/reference/error-codes/) or one
of the `E_CLI_*` codes the CLI raises itself. `next_action` is one line of
advice built from Reqloom's own data, never from an API response. `file` and
`line` are set when the error is in a project file. (`run`'s error document has
the shape described in [`reqloom run`](/cli/run/#json).)

## stdout vs stderr

Deliberately split so you can pipe one without the other:

- **stdout** — the rendered result: the summary table, or the JSON/JUnit
  document, or `import`'s success line and the names of the credentials it moved
  into secrets.
- **stderr** — failures and diagnostics: failed steps, schema errors, lint's
  `WARNING:` lines, and `import`'s review notes and kept-credential warnings.

Failed steps print to stderr **even under `--quiet`**, so a CI log always shows
what broke:

```bash
reqloom run order.pay --quiet > /dev/null    # stderr still reports failures
```

## Next

- [`reqloom run`](/cli/run/) — output formats, variable overrides, CI recipes
- [`reqloom lint`](/cli/lint/) — what it checks, and what it can't
- [`reqloom plan`](/cli/plan/) and [`reqloom show`](/cli/show/) — before and after a run
- [`reqloom import`](/cli/import/) — the nine formats it accepts
