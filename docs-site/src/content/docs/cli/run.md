---
title: reqloom run
description: "Execute a chain ending at one operation. Flags, the three output formats, variable overrides, and how failures are reported."
---

Resolves the dependency chain for one operation, then executes it in
topological order.

```bash
reqloom run <resource.operation> [options]
```

The operation id is `<resource>.<operation>` — the resource name from your
schema, not the filename. `resources/orders.yaml` declaring `name: order` gives
you `order.create`, not `orders.create`.

**The operation must come first.** Flags are parsed after it, so
`reqloom run --env staging order.create` fails with `unknown argument`.

## Options

| Flag | Value | Default |
| --- | --- | --- |
| `--project <path>` | Project directory | current directory |
| `--env <name>` | Environment to run against | the schema's `default_environment` |
| `--var KEY=VALUE` | Override one environment variable. Repeatable | — |
| `--format <fmt>` | `text`, `json`, or `junit` | `text` |
| `--output <file>` | Write the rendered result to a file | stdout |
| `--quiet` | Suppress live progress | off |
| `--help` / `-h` | Show usage | — |

There are no short forms other than `-h`, and no `--dry-run`, `--timeout`,
`--retries`, or `--verbose`. Timeouts and retries are declared per operation in
the schema, not per invocation.

## What a run looks like

```bash
reqloom run refund.approve --project samples/marketplace
```

```ansi
Loaded project: MarketplaceAPI (3 actors, 5 resources)
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create (attempt 1)
  [2] Running: product.publish (attempt 1)
  [3] Running: cart.add_item (attempt 1)
  [4] Running: order.create (attempt 1)
  [5] Running: order.pay (attempt 1)
  [6] Running: refund.request (attempt 1)
  [7] Running: refund.approve (attempt 1)

Result: SUCCEEDED

--- Chain Summary ---
Target: refund.approve   Env: local   Outcome: SUCCEEDED
  OK     product.create (113ms) err=—
  OK     product.publish (79ms) err=—
  OK     cart.add_item (94ms) err=—
  OK     order.create (164ms) err=—
  OK     order.pay (402ms) err=—
  OK     refund.request (97ms) err=—
  OK     refund.approve (121ms) err=—
```

You asked for one operation and got seven, across three actors, because
`refund.approve` needs a refund, which needs a paid order, which needs a cart
item, which needs a published product. Each actor's login happens automatically
and does **not** appear as a chain step — logins are part of the step that
needs them.

### Status codes in the summary

| Glyph | Meaning |
| --- | --- |
| `OK` | Succeeded |
| `FAIL` | Failed — see the `err=` code |
| `SK` | Skipped, already satisfied earlier in this run |
| `BLOCK` | Never ran because something upstream failed |
| `CANCEL` | Run was cancelled |

## When something fails

The failing step prints to stderr, and everything downstream is marked
`BLOCK` rather than attempted:

```ansi
Running: refund.approve (chain of 7 steps, env=local)
  [1] Running: product.create (attempt 1)
  [1] FAILED: product.create [E_SESSION_REFRESH_FAILED] —

Result: FAILED

--- Chain Summary ---
Target: refund.approve   Env: local   Outcome: FAILED
  FAIL   product.create (0ms) err=E_SESSION_REFRESH_FAILED
  BLOCK  product.publish (0ms) err=—
  BLOCK  cart.add_item (0ms) err=—
  BLOCK  order.create (0ms) err=—
  BLOCK  order.pay (0ms) err=—
  BLOCK  refund.request (0ms) err=—
  BLOCK  refund.approve (0ms) err=—
```

That's the real output when the vendor's login can't reach the server: step 1
fails on auth, and the other six never run. `BLOCK` is your signal to fix the
first failure and re-run rather than debug seven things.

Exit code is `1`. The `E_*` code tells you the class of problem — see
[error codes](/reference/error-codes/).

## Output formats

### text (default)

Human-readable. Live progress goes to stdout as the run unfolds; the summary
table follows. `--quiet` suppresses the progress lines but **still prints the
summary table**, because a silent successful run is useless.

### json

For scripts and dashboards.

```bash
reqloom run refund.approve --format json
```

```json
{
  "run_id": 1,
  "target": "refund.approve",
  "environment": "local",
  "outcome": "FAILED",
  "summary": {
    "succeeded": 0,
    "failed": 1,
    "skipped": 0,
    "blocked": 6,
    "cancelled": 0,
    "for_each_iterations": 0,
    "poll_attempts": 0
  },
  "steps": [
    {
      "op": "product.create",
      "status": "FAIL",
      "attempts": 1,
      "elapsed_ms": 0,
      "error_code": "E_SESSION_REFRESH_FAILED",
      "poll_attempt": null,
      "for_each_index": null,
      "detail": "",
      "assertions": []
    }
  ]
}
```

`status` uses the same short glyphs as the text summary (`OK`, `FAIL`, `SK`,
`BLOCK`, `CANCEL`) — not the long names. `poll_attempts` and
`for_each_iterations` are counted separately in `summary` and excluded from the
status totals, so `succeeded + failed + skipped + blocked + cancelled` equals
the number of real steps.

### junit

For CI test reporting. GitHub Actions, GitLab, and Jenkins all read this.

```bash
reqloom run checkout.complete --format junit --output results.xml
```

```xml
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="refund.approve" tests="7" failures="1" errors="6" skipped="0" time="0">
  <testsuite name="reqloom.refund.approve" tests="7" failures="1" errors="6" skipped="0" time="0" hostname="reqloom-cli">
    <properties>
      <property name="reqloom.target" value="refund.approve"/>
      <property name="reqloom.environment" value="local"/>
      <property name="reqloom.outcome" value="FAILED"/>
      <property name="reqloom.run_id" value="1"/>
    </properties>
    <testcase classname="reqloom.refund.approve" name="product.create" time="0">
      <failure type="E_SESSION_REFRESH_FAILED" message=""/>
    </testcase>
    <testcase classname="reqloom.refund.approve" name="product.publish" time="0">
      <error type="BLOCKED" message="upstream step failed; this step did not run"/>
    </testcase>
  </testsuite>
</testsuites>
```

Note the mapping: a failed step becomes `<failure>`, while **blocked and
cancelled steps become `<error>`**. Skipped steps become `<skipped>`. Polling
attempts and for-each iterations fold into their parent `<testcase>` as
`<system-out>` instead of inflating the test count.

:::note[json and junit imply --quiet]
Both formats emit a single document on stdout, so live progress is suppressed
automatically — otherwise the progress lines would corrupt the document. You
don't need to pass `--quiet` yourself.
:::

## Overriding variables

`--var` replaces one environment variable for this run only. Nothing is written
to disk.

```bash
reqloom run order.create --var baseUrl=https://staging.example.com
reqloom run order.create --var coupon=SAVE20 --var quantity=3
```

`--var` needs `KEY=VALUE`; a bare key is exit code `2`:

```ansi
reqloom run: --var requires KEY=VALUE, got 'coupon'
```

Overrides apply to the environment selected by `--env`, creating the variable if
the environment doesn't declare it. Use them for per-run values (a coupon code,
a target host) rather than secrets — real secrets belong in the keychain, via
[`!secret`](/schema/secrets-and-transport/).

## Switching environments

```bash
reqloom run order.create --env staging
```

With no `--env`, the schema's `default_environment` is used (itself `local` if
unset).

:::caution[A misspelled environment does not error]
If `--env` names an environment your schema doesn't define, the run proceeds
with **zero variables** rather than failing. Every `{{env.X}}` is then
unresolved, which usually surfaces as a confusing URL or a 404 rather than a
clear error. Check the `Env:` line in the summary if a run behaves oddly.
:::

## In CI

```yaml title=".github/workflows/api-tests.yml"
- name: Run API workflow tests
  run: |
    reqloom run checkout.complete \
      --project api-tests \
      --env staging \
      --format junit \
      --output results.xml

- name: Publish results
  if: always()
  uses: actions/upload-artifact@v4
  with:
    name: reqloom-results
    path: results.xml
```

Exit code `1` fails the step on a broken API. Codes `2` and `3` mean the
invocation itself is wrong — see [exit codes](/cli/overview/#exit-codes).

## Troubleshooting

### The run takes minutes against a server that isn't up

Expected, and worth understanding. Each step retries **3 times** by default, and
each attempt waits out the connect timeout (5s default). A seven-step chain
against a dead host is therefore several minutes of mostly waiting.

Shorten the connect timeout while developing offline:

```yaml title="environments/local.yaml"
transport:
  connect_timeout: 100ms
```

### `E_REF_UNDEFINED` naming an operation I'm sure exists

The resource id comes from `name:` inside the file, not the filename:

```ansi
Engine error [E_REF_UNDEFINED]: Resource not found: nope (referenced by operation nope.missing)
```

Run [`reqloom lint`](/cli/lint/) — it prints the actor, resource, and operation
counts it found, which is the quickest way to confirm what the parser sees.

### A step is `SK` when I wanted it to run again

The engine reuses anything already satisfied in this run. Force a specific
operation to always execute with `force: true` in its schema entry.
