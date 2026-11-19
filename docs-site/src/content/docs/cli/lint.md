---
title: reqloom lint
description: "Validate a schema without sending requests. What lint catches, what it only warns about, what it lets through, and how to wire it into CI."
---

Validates a project and dry-runs every operation. No HTTP requests are sent, no
secrets are read from your keychain, so it's safe in CI and safe offline.

```bash
reqloom lint [<project-dir>] [--format text|json]
```

The project is the positional argument or `--project <path>` (one or the other),
and defaults to the current directory. An unknown option is exit code `2`.
`reqloom lint --help` lists the options.

## JSON output

`--format json` writes one document to stdout. Warnings still go to stderr as
well.

```json
{
  "schema_version": 1,
  "contract_version": "1.2",
  "command": "lint",
  "ok": false,
  "errors": [
    {"code": "E_REF_UNDEFINED", "detail": "./resources/refunds.yaml:23: Operation 'refund.approve' names actor 'amdin', which is not defined", "operation": null, "file": "resources/refunds.yaml", "line": 26, "column": null, "reference": "amdin", "fix": "Name one of the defined ones: admin, customer, vendor.", "next_action": "Edit the file at the line given (file, line), doing what fix says, then run `reqloom lint` again."}
  ],
  "warnings": [],
  "warning_details": []
}
```

Each error carries:

| Field | Meaning |
| --- | --- |
| `code`, `detail` | The `E_*` code and the message, as before. `detail` keeps its `file:line` prefix, which names the operation's line |
| `operation` | Set when the error belongs to one operation's chain |
| `file` | The schema file, relative to the project folder; `null` when the error is not in a file |
| `line`, `column` | 1-based; the line of the field that holds the problem (the `actor:` line, the header or `depends_on` item, the `path:` or `body:` key). `null` when unknown |
| `reference` | The offending reference or name as written: `{{order.owner_id}}`, `amdin`, `order.nope` |
| `fix` | What would satisfy the check, e.g. the names that do exist |
| `next_action` | One line of advice for an agent |

`warnings` stays an array of strings; `warning_details` has one object per
warning, at the same index: `{ message, file, line, column, reference, fix }`.

### How far positions reach

| Problem | file | line | column |
| --- | --- | --- | --- |
| YAML syntax, wrong type or enum value, duplicate ids | yes | yes | yes |
| Unknown scope (`{{ghost.id}}`), `$.env` not in `allowed_env`, unknown `$.faker` | yes | the field's line | — |
| Unknown actor, `depends_on` target, `for_each` resource | yes | the field's line | — |
| Ambiguous or missing producer of a value | yes | the field's line | — |
| Malformed assertion, poll condition or regex | yes | the item's line | — |
| A cycle | the first operation in it | its `depends_on` line | — |
| A reference nothing extracts (warning) | yes | the field's line | — |
| Unknown keys and other load warnings | yes | yes | yes |
| Secret-name and project-id warnings | `reqloom.yaml` or none | — | — |

A reference inside a multi-line `body:` points at the `body:` line: the body is
read as one value. Lint stops at the first load error; fix it and run lint again
to see the next.

## A clean run

```bash
reqloom lint --project samples/marketplace
```

```ansi
LINT OK — 3 actors, 5 resources, 27 operations. No errors.
```

Those counts are the fastest way to confirm the parser sees what you think it
sees. If a resource you just added isn't in the total, its file isn't being
imported — check the `imports:` globs.

Exit code is `0`.

## Warnings

Some problems are reported without failing the lint. Each is a `WARNING:` line
on stderr, before the result line, and the exit code stays `0`:

```ansi
  WARNING: ./resources/empty.yaml: file is empty; ignored
  WARNING: ./resources/users.yaml:3:34: unknown key 'colour' in operation 'users.list'; ignored
  WARNING: ./reqloom.yaml:6:5: imports pattern 'environments/*.yaml' matched no files
LINT OK — 1 actors, 1 resources, 1 operations. No errors.
```

| Warning | Means |
| --- | --- |
| `unknown key '…'` | A key the parser doesn't read — usually a typo. It is ignored |
| `imports pattern '…' matched no files` | A glob in `imports:` found nothing |
| `imported file … is not under actors/, resources/ or environments/` | The file is ignored |
| `file is empty; ignored` | An imported file has no content |
| `unknown _provenance source '…'` | Importer metadata this build doesn't know; kept as written |
| `project uses secrets but has no id` | Its secrets cannot be read from the keychain (the CLI still reads `REQLOOM_SECRET_<NAME>` variables); the line suggests an id |
| `secrets '…' and '…' both map to the environment variable …` | Two names differing only in case share one `REQLOOM_SECRET_` variable; a CLI run with it set fails. Rename one |
| `secret '…' cannot be supplied through an environment variable` | The name has a character other than a letter, digit or `_`, so no [`REQLOOM_SECRET_<NAME>`](/schema/secrets-and-transport/#on-ci-and-in-agent-sandboxes-environment-variables) variable can carry it; on CI it has no source. The line suggests a name |
| `Operation '…' refers to {{r.x}}, but no operation of 'r' extracts 'x'` | A typo in an extracted value's name, or a missing `extract:`; the run fails when it reaches it. `fix` lists what `r` does extract |

Warnings are printed only by `reqloom lint`, and only when the project loads; a
load error is printed alone. `reqloom run` and the desktop app don't show them.
To fail CI on warnings, read them from the JSON document:

```bash
reqloom lint api-tests --format json 2>/dev/null | jq -e '.ok and (.warnings | length == 0)'
```

Messages give the file as Reqloom opened it: relative paths such as
`./reqloom.yaml` when you pass `--project .`, full paths when you don't.

## What it checks

Lint does more than parse YAML. After loading the schema it resolves a chain for
**every** operation in dry-run mode, which is what catches dependency problems a
pure syntax check would miss:

1. **YAML syntax** across the root file and every imported file
2. **Schema version** — must be 1–3; a present `id:` must be valid and
   `allowed_env:` must be a list
3. **Types and values** — each key has the right type (`timeout: 30s` is not an
   integer); `method`, `auth.strategy`, inline `auth.type`, extraction `source`
   and other enums hold an accepted value; durations and sizes are well formed;
   `assert` is a list and every `for_each` has `over`
4. **Ids** — no actor, resource or environment is defined twice, and none is
   empty, starts with `.`, or contains `/`, `\` or `:`
5. **Reference targets** — every `{{X.y}}` names a real actor, resource,
   environment variable, or secret, including references inside function calls
   such as `{{$.base64.encode(order.order_id)}}` and inside assertions and poll
   conditions
6. **Named things exist** — an operation's or poll's `actor`, a `for_each`
   resource, and every `depends_on` target
7. **Builtins** — `$.faker` names are `email` or `phone`; every `$.env` name, in
   operations, environment values and actors, is listed in `allowed_env`
8. **Expressions** — every assertion and poll condition parses (with each
   `{{…}}` standing in as one value), every regex (extraction or `matches`)
   passes the [regex check](/schema/advanced-operations/#regular-expressions),
   and every `failure_message.show` path is a supported JSONPath
9. **One producer per value** — when several operations extract the same
   variable, a reference to it has one clear
   [producer](/concepts/dependencies/#when-several-operations-extract-the-same-value)
10. **No dependency cycles**
11. **Every operation's chain resolves** to an executable order

A load error starts with `file:line:col` (or `file:line` for a reference or
name check) and, for an enum, lists the accepted values:

```ansi
LINT FAIL [E_SCHEMA_INVALID]: ./reqloom.yaml:11:17: unknown method 'PSOT'; accepted values: GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS
```

A project that references secrets but has no `id:` lints with a warning, not an
error — a real run would fail:

```ansi
  WARNING: project uses secrets but has no id; runs will fail. Add `id: 6f0c…` to reqloom.yaml.
```

### Undefined reference

```ansi
LINT FAIL [E_REF_UNDEFINED]: ./reqloom.yaml:10: Operation 'order.get' references undefined symbol 'invoice.invoice_id': no actor, resource, env, or secret named 'invoice'
    at reqloom.yaml:12
    reference: {{invoice.invoice_id}}
    fix: Refer to an actor (admin), a resource (order), env.<key>, secret.<NAME> or a $. built-in.
```

The `at`, `reference` and `fix` lines follow every error whose position, reference
or fix is known; `at` names the line of the field, while the prefix of the
message keeps the operation's line.

### Missing dependency

```ansi
LINT FAIL [E_REF_UNDEFINED]: ./reqloom.yaml:10: Operation 'order.one' declares depends_on 'ghost.op', which is not a defined operation
```

### Unknown actor

```ansi
LINT FAIL [E_REF_UNDEFINED]: ./reqloom.yaml:10: Operation 'order.create' names actor 'ghost', which is not defined
```

### Malformed assertion

```ansi
LINT FAIL [E_SCHEMA_INVALID]: ./reqloom.yaml:10: Operation 'order.one' assert[0]: predicate: expected identifier after '.' at position 3
```

### Cycle

```ansi
LINT FAIL [E_CYCLE]: Circular dependency detected: order.one → order.two
```

### Unsupported version

```ansi
LINT FAIL [E_SCHEMA_VERSION]: Unsupported schema version 9 (supported: 1–3). Run `reqloom migrate` to upgrade.
```

The message suggests `reqloom migrate`, which is not a command in this release.
Set `version:` to 1, 2 or 3.

A project that doesn't validate is exit code `1`. Per-operation failures are listed individually,
then totalled:

```ansi
  ERROR order.pay: [E_VAR_UNRESOLVED] Required variable couldn't be substituted

LINT FAILED — 1 error(s).
```

## What it does not check

Lint validates structure, not correctness against a live API. These gaps are
worth knowing, because each produces a green lint and a red run.

:::caution[A misspelled extracted value is a warning, not an error]
`{{order.missing_var}}` loads as long as a resource named `order` exists, so lint
still passes. Lint **warns** when no operation of `order` extracts `missing_var`,
with the line, the reference and the names that are extracted; the run fails
with `E_VAR_UNRESOLVED` when it gets there.
:::

Also invisible to lint:

- **Whether the API behaves.** Wrong paths, wrong bodies, and wrong
  `expect_status` values all lint clean and fail on first contact.
- **Misspelled keys, as errors.** A key the parser doesn't recognise —
  `expect_stats: 200`, `dependson:` — is a warning and is ignored; lint still
  passes. See [warnings](#warnings).
- **Whether secrets exist, or belong to this folder.** Dry runs deliberately skip
  the keychain, so a missing `!secret` entry or a
  [folder conflict](/schema/secrets-and-transport/#each-project-id-belongs-to-one-folder)
  only appears at run time.
- **Extraction JSONPaths.** `$..name` in an `extract:` path lints clean and fails
  the step at run time with `E_EXTRACTION_FAILED`.
- **What a placeholder in a predicate resolves to.** The check confirms the
  scope exists and that the predicate parses with a stand-in value; a field that
  is never extracted is a [warning](#warnings) and fails at run time with
  `E_VAR_UNRESOLVED`.
- **A missing `imports:` block.** Without it no file is read and there is
  nothing to warn about: lint reports `0 resources` and passes.

## In CI

Lint is fast and needs no network, so run it on every push before any real
requests:

```yaml title=".github/workflows/api-tests.yml"
- name: Validate schema
  run: reqloom lint --project api-tests
```

As a pre-commit hook:

```bash title=".git/hooks/pre-commit"
#!/usr/bin/env bash
set -euo pipefail
reqloom lint --project api-tests
```

Exit code `1` means the schema needs attention; `2` means the command itself is
wrong (an unknown option, or no `reqloom.yaml` at that path).

## Next

- [Common pitfalls](/schema/pitfalls/) — what lint warns about and what it misses
- [Error codes](/reference/error-codes/) — what each `E_*` means
- [`reqloom run`](/cli/run/) — execute the chain once it lints clean
