---
title: reqloom lint
description: "Validate a schema without sending requests. What lint catches, what it silently lets through, and how to wire it into CI."
---

Validates a project and dry-runs every operation. No HTTP requests are sent, no
secrets are read from your keychain, so it's safe in CI and safe offline.

```bash
reqloom lint [--project <path>]
```

`--project` defaults to the current directory. **It takes no other flags** —
unknown arguments are silently ignored rather than rejected, and there is no
`lint --help`. `reqloom lint --format json` runs a perfectly normal lint and
throws the flag away.

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

## What it checks

Lint does more than parse YAML. After loading the schema it resolves a chain for
**every** operation in dry-run mode, which is what catches dependency problems a
pure syntax check would miss:

1. **YAML syntax** across the root file and every imported file
2. **Schema version** — must be 1–3
3. **Reference targets** — every `{{X.y}}` names a real actor, resource,
   environment variable, or secret
4. **`depends_on` targets** exist as operations
5. **No dependency cycles**
6. **Every operation's chain resolves** to an executable order

### Undefined reference

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'order.get' references undefined symbol
'invoice.invoice_id': no actor, resource, env, or secret named 'invoice'
```

### Missing dependency

```ansi
LINT FAIL [E_REF_UNDEFINED]: Operation 'order.one' declares depends_on 'ghost.op',
which is not a defined operation
```

### Cycle

```ansi
LINT FAIL [E_CYCLE]: Circular dependency detected: order.one → order.two
```

### Unsupported version

```ansi
LINT FAIL [E_SCHEMA_VERSION]: Unsupported schema version 9 (supported: 1–3).
Run `reqloom migrate` to upgrade.
```

Any failure is exit code `1`. Per-operation failures are listed individually,
then totalled:

```ansi
  ERROR order.pay: [E_VAR_UNRESOLVED] Required variable couldn't be substituted

LINT FAILED — 1 error(s).
```

## What it does not check

Lint validates structure, not correctness against a live API. Four gaps are
worth knowing, because each produces a green lint and a red run.

:::caution[Lint validates the scope, not the field]
`{{order.missing_var}}` **passes** lint as long as a resource named `order`
exists. The validator confirms the *scope* is defined; it does not verify that
some upstream operation actually extracts `missing_var`. A typo in an extracted
variable name is a run-time `E_VAR_UNRESOLVED`, not a lint error.
:::

Also invisible to lint:

- **Whether the API behaves.** Wrong paths, wrong bodies, and wrong
  `expect_status` values all lint clean and fail on first contact.
- **Misspelled keys.** The parser ignores keys it doesn't recognise, so
  `expect_stats: 200` or `dependson:` is silently dropped. See
  [common pitfalls](/schema/pitfalls/).
- **Whether secrets exist.** Dry runs deliberately skip the keychain, so a
  missing `!secret` entry only appears at run time.

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

Because a broken invocation and a broken schema both exit non-zero here (lint
can't return `2`), a failing hook always means the schema needs attention.

## Next

- [Common pitfalls](/schema/pitfalls/) — the silent failures lint won't catch
- [Error codes](/reference/error-codes/) — what each `E_*` means
- [`reqloom run`](/cli/run/) — execute the chain once it lints clean
