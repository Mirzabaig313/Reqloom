---
title: Desktop app
description: "The Reqloom workbench: explorer, editor, and response panels, with the chain always visible. What it does, and when to use it instead of the CLI."
---

The desktop app and the CLI drive the same engine and read the same YAML. A schema
that lints in one behaves identically in the other — so you can explore in the
app and run in CI without translating anything.

The app is where you **build and debug** a schema. The CLI is where you **run** it.

## Which one to use

| Task | Use |
| --- | --- |
| Explore an unfamiliar API | Desktop |
| Write or edit a schema without hand-editing YAML | Desktop |
| Debug why a step failed, step by step | Desktop |
| Store a credential in your OS keychain | **Desktop only** — the CLI can't |
| Inspect a response body, diff it against a saved example | Desktop |
| Run in CI, or from a script | **CLI only** |
| Machine-readable output (JSON, JUnit) | **CLI only** |
| Validate a schema in a pre-commit hook | CLI |
| Re-run the same chain repeatedly while changing one field | Desktop |

Neither is a subset of the other. Most people use both: the app to get a chain
working, the CLI to keep it working.

## The workbench

Three panes, left to right:

```
┌──────────────┬─────────────────────────────────┬──────────────┐
│              │  tabs                           │              │
│  Explorer    │  chain strip                    │  Response    │
│              │ ┌─────────────────────────────┐ │      or      │
│  actors      │ │ GET  /api/v1/orders/{{id}}  │ │  Timeline    │
│  resources   │ │                    [Send]   │ │              │
│  operations  │ ├─────────────────────────────┤ │  body tree   │
│              │ │ Params Headers Body Auth    │ │  raw · diff  │
│              │ │ Chain  Assertions           │ │  headers     │
│              │ └─────────────────────────────┘ │              │
└──────────────┴─────────────────────────────────┴──────────────┘
```

Both side panes are **resizable and collapsible**, and the layout persists
between sessions:

| Action | How |
| --- | --- |
| Collapse / show Explorer | `Ctrl+B`, or View → Hide Explorer |
| Collapse / show Response | `Ctrl+J`, or View → Hide Response |
| Resize | Drag a splitter |
| Response beside vs below | Response header → Split Horizontally / Vertically |

A collapsed pane becomes a thin labelled rail — click it to bring the pane back.
By default the Response pane moves **below** the editor on a narrow window and
back beside it when there's room; View → Automatic Response Layout controls that.

## Explorer

Your project as a tree: actors and resources, with each resource's operations
beneath it, and any saved response examples beneath those.

- **Click** an operation to open it
- **Double-click** (or `Enter`) to open **and run** it
- **Right-click** for Edit, Rename, Delete, New Endpoint, New Module, New Actor
- **Search** filters operations by name and method across every open project

Two status dots are worth knowing:

- Next to an **actor** — whether its session is live, expiring, or absent
- Next to an **operation** — how it did on its last run

## Editor tabs

Several operations stay open at once, like an editor. Tabs are drag-reorderable;
middle-click or `×` closes one, and `Ctrl+W` closes the active tab. Tabs show the
HTTP method, so a row of them reads as a workflow.

## The chain strip

Above the editor, always visible, is the resolved chain for whatever you have
open. Each step is a pill you can click to jump to that operation, so a seven-step
chain is navigable rather than described.

It **hides itself** for an operation with no dependencies, so a plain request
loses no vertical space.

There's also **Run cleanly** here — discard cached sessions and extractions, then
run the whole chain from scratch. That's the button for "it worked once and now I
don't trust the cache".

## Working with projects

| Action | Where |
| --- | --- |
| New project | File → New Project, creates `reqloom.yaml` |
| Open project | File → Open Project, pick the folder |
| Import a spec | File → Import — OpenAPI, Postman, Insomnia, Thunder Client, Hoppscotch, REST Client, Bruno |
| Switch project | Project switcher in the toolbar, with a Recent list |

Several projects can be open at once; the switcher moves between them.

## Keyboard

| Shortcut | Does |
| --- | --- |
| `Ctrl+Enter` | Run the current operation |
| `Ctrl+P` | Command palette |
| `Ctrl+B` | Toggle Explorer |
| `Ctrl+J` | Toggle Response |
| `Ctrl+W` | Close the active tab |
| `Enter` in Explorer | Run the selected operation |

The command palette covers New Project, Open, Import, Manage Secrets, View
Cookies, New Module, New Endpoint, Run, Dry Run, Generate Hook Typings, and the
light/dark switch.

## Appearance

Appearance menu, or the sun / moon / monitor buttons in the toolbar:

- **Light**, **Dark**, or **System**
- Density: **Comfortable** or **Compact**

Both persist. There's no separate preferences window — settings live in the menus
and toolbar.

## Capture bodies

A toolbar checkbox. When on, response bodies are kept in memory so you can browse
the body tree, diff against a saved example, and save examples. When off, the raw
view tells you the body wasn't captured.

Leave it on while developing. Turn it off when running something that returns very
large payloads.

## Next

- [Running & debugging](/desktop/running/) — dry runs, the timeline, history, latency
- [Editing schemas](/desktop/editing/) — edit mode, the chain table, secrets, environments
- [CLI overview](/cli/overview/) — the other half
