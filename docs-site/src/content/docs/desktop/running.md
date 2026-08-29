---
title: Running & debugging
description: "Dry runs, chain previews, the step timeline, unresolved-variable diagnostics, persisted run history, and latency charts."
---

Four ways to run, and a timeline built for working out why a step failed.

## Running

| How | Does |
| --- | --- |
| **Send** in the address bar | Run the open operation and its chain |
| `Ctrl+Enter` | Same |
| Double-click in the Explorer | Open and run |
| Command palette → Run | Same |

While a run is in flight, **Send** reads *Running…* and the Timeline header offers
**Cancel**.

### Dry run

**More ▾ → Dry Run — preview without sending.** Resolves the whole chain, works
out the order, and reports what *would* happen without sending a request.

Use it when you've just wired up dependencies and want to check the shape before
touching a real API. It also never touches your keychain, so it won't prompt for
an unlock.

### Fresh sessions

Two related but distinct controls:

| Control | Where | Does |
| --- | --- | --- |
| **Run cleanly** | Chain strip | Discard cached sessions + extractions, then run |
| **Send (fresh session)** | More ▾ | Same, from the editor |
| **Reset caches** | Timeline header | Clear the caches *without* running |

Reach for these when a run succeeds but you suspect it's reusing a stale token or
a record from a previous run.

## Seeing the chain before you run

Two previews, both live:

- **The chain strip** above the editor — the resolved step order, clickable.
- **The Response panel's EXECUTION PATH** — shown when nothing has run yet. Each
  step with its actor, the variables it produces, and its expected status.

In the preview, `{{variables}}` are deliberately left **unresolved**. That's not a
bug: nothing has run, so there's no value yet, and seeing the reference tells you
where the value is meant to come from.

For a visual view, the editor's **Execution chain** section embeds a layered
dependency graph — prerequisites above dependents, zoomable, and clicking a node
shows its path, actor, extractions, and dependencies, with **Open endpoint** and
**Edit chain** actions.

## The timeline

Every step of the last run, expandable. Per step you get the request, the response,
what was extracted, and on failure the error and its `E_*` code.

Two navigation aids that make a long chain tractable:

- **"needed by" chips** — which later steps consume this step's values. Click one
  to jump there.
- **"Show blocking step N"** — on a blocked step, jumps to the failure that
  actually stopped it.

That second one matters because a failure produces one real error and a run of
`BLOCK` steps that never executed. The timeline points you at the cause rather
than the symptoms.

### Unresolved variable diagnostics

The most useful thing in the app. When a reference doesn't resolve, the step shows
a diagnostics block naming the variable and offering:

- **Open request field** — jump to the field containing the reference
- **Show producer step N** — jump to the step that was supposed to produce it
- **Edit source** — jump straight to the thing that's missing: the environment
  variable, the secret, the actor, or the upstream extraction

So `{{order.typo}}` goes from a run-time failure to a two-click fix, instead of a
hunt through YAML. This is the class of bug [lint cannot
catch](/cli/lint/#what-it-does-not-check), because the scope exists and only the
field is wrong.

## Inspecting the response

Sub-tabs: **Body (Tree)**, **Body (Raw)**, **Headers**, **Diff**.

In the tree, hovering a row gives you:

- **copy path** — the JSONPath to that node
- **copy value**
- **＋ save as variable** — create an extraction from the node you clicked

That last one is the fastest way to wire a chain: run a request, find the id in the
response, click save-as-variable, and it becomes an extraction another operation
can reference.

**Save example** stores a response under a name. Later runs can then use **Diff**
to line-diff the current response against that example — which is how you catch an
API changing shape rather than merely breaking.

Both the tree and Diff need **Capture bodies** on.

## Run history

Persisted in SQLite, per project, and it survives restarts. **History** in the
toolbar lists the last 100 runs — status, target, environment, timestamp, step
count, duration — over a bar chart of durations across runs, so a slowdown is
visible.

**Clicking a run replays its timeline** into the Timeline panel: the same
per-step request, response, and extraction detail as when it ran. You can debug a
failure from yesterday without reproducing it.

**Clear** wipes history for the project.

## Latency

Above the timeline, a chart of the **current run's** response times, in three
views: scatter, bars, or histogram. It shows median, p95, p99, and max, with a
dashed p95 marker.

Clicking a bar scrolls the timeline to that step — useful for "which step was the
slow one" on a long chain.

You can set a **p95 SLO** inline (**+ SLO**, or click the chip to change it). It's
saved to the project, and a breach turns the marker and summary red. That's the
`latency_slo.p95_ms` key in `reqloom.yaml`, so the CLI and the app agree on it.

## Cookies

**Command palette → View Cookies** opens a per-actor cookie-jar inspector — worth
knowing about for APIs that authenticate with cookies rather than bearer tokens,
where "why am I getting a 401" is often a cookie question.

## Next

- [Editing schemas](/desktop/editing/) — turning findings into saved changes
- [Error codes](/reference/error-codes/) — what each `E_*` in the timeline means
- [`reqloom run`](/cli/run/) — the same execution, in CI
