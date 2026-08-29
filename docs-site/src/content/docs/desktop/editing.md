---
title: Editing schemas
description: "Edit operations, chains, actors, environments, and keychain secrets from the app — and what happens to your YAML when you save."
---

The app is fully read-write. Everything you can express in YAML you can edit here,
and saving writes the YAML files back.

## Read mode and edit mode

An open operation starts **read-only**, with tabs showing Params, Headers, Body,
Auth, Chain, and Assertions as they are on disk.

Click **✎ Edit** to change it. Any empty read-mode tab also offers **＋ Add
parameter / header / variable / assertion**, which flips into edit mode for you.

In edit mode you get three buttons and one important distinction:

| Button | Does |
| --- | --- |
| **Send** | Runs with your edits, **without saving them** |
| **✓ Save** | Writes the edits to the project |
| **Cancel** | Discards them |

The banner spells it out: *Unsaved edits — Send runs them once; Save commits to
the project.* That's the loop you want while debugging — change a header, Send,
look at the response, change it again, and only Save once it works.

Tab labels show dirty counts (`Params 3`, `Body ●`) so you can see what you've
touched.

## What you can edit

| Tab | Covers |
| --- | --- |
| **Params** | Query parameters |
| **Headers** | Request headers, plus a read-only list of auto-generated ones |
| **Body** | none / form-data / x-www-form-urlencoded / JSON / XML / Text / GraphQL |
| **Auth** | Pick an actor, or set inline auth for this one endpoint |
| **Options** | Expected status codes, timeout, force re-run |
| **Chain** | Dependencies and extractions for every step |
| **Assertions** | Response assertions, evaluated live |

Some details worth knowing:

- **Body** has a **Beautify** button in edit mode that reformats for real. The
  read-mode Beautify is display-only — it never changes the saved body.
- **form-data** supports file attachments, and **Check upload** reports the
  multipart part count and total size before you send.
- **Assertions** evaluate live against the last response, showing ✓ / ✗ per row, so
  you can write one and immediately see whether it holds.
- **Auth** offers the inline types — API Key, Bearer, Basic, OAuth 2.0
  (auth-code + PKCE, client credentials, password), JWT, AWS SigV4, mTLS, OAuth 1.0
  — plus **Save as project default** for a credential every endpoint should
  inherit.

## Editing the whole chain at once

The **Chain** tab is the most useful editor in the app. It shows **every step** in
the current chain as a row, and for each one lets you edit:

- Its **extractions** — save-as name and the response path, with the value
  evaluated live so you can see `= ord_123` as you type
- Its **run mode** — once, or **for each** item of an upstream list (data-driven
  fan-out), with a "keep going if an item fails" option
- **+ Add dependency** — pick another operation

**Save chain** commits changes across all those operations in one write. So
rewiring a seven-step chain is one screen and one save, not seven files.

Extraction paths autocomplete from the **actual response** of that step, so you
pick real JSONPaths rather than guessing them.

## Autocomplete

Three places the app knows more than you do:

| Where | Suggests |
| --- | --- |
| Path field, raw body | `{{variables}}` that are actually in scope at that point |
| Extraction path fields | Real JSONPaths from that step's captured response |
| Header key fields | Common HTTP header names |

The variable autocomplete triggers inside an unclosed `{{`, and only offers
references the engine says resolve there — which prevents the most common schema
bug at the point of typing.

## Actors

Select an actor in the Explorer to view it; **Edit** turns the same panel into a
form. You can set the strategy and its per-strategy fields, refresh settings, and
build a **multi-step login chain** — adding, removing, and reordering steps, each
with its own method, path, body, expected status, and extractions.

New actors use the same panel, opened straight in edit mode.

## Secrets

**This is the only way to store a secret** — the CLI has no command for it.

**Manage Secrets** in the toolbar (or File menu, or the palette) lists every
`{{secret.NAME}}` your project references, each with its keychain status:

- **Set…** stores a value in the OS keychain, entered masked
- **Clear** removes it

Values are **never displayed back** — only whether each is set. The list is derived
from your schema, so a secret appears here once something references it; you don't
add names by hand.

If your build has no keychain backend, a red banner says so and writes won't
persist.

## Environments

**Manage Environments** from the environment selector. Create environments, set the
**Base URL** (`{{env.baseUrl}}`), and edit variables as a key/value table.

The selector in the toolbar switches the active environment, and the choice is
remembered. One caveat: switching rows inside the manager discards unsaved edits to
the pane you were on — save before moving.

## Hooks

**Hooks…** opens the pre-request / post-response JavaScript editor. It's a separate
window with a code editor rather than part of the main panel — expected, not a
glitch.

**Generate Hook Typings** in the palette writes a `reqloom.d.ts` so your editor can
autocomplete the hook API.

## What saving does to your YAML

Worth understanding before you edit a hand-written schema.

Saving **re-emits** the affected file from the in-memory model. Before writing, the
whole project is validated — a cycle or an undefined reference is rejected and
nothing is written, so a bad edit can't leave an unloadable project.

:::caution[Comments are lost in files that change]
The writer regenerates YAML; it does not preserve comments or key order in a file
it rewrites. It **does** byte-compare every file and skip unchanged ones, so
comments in the rest of your project survive — only the file you actually edited
loses them.

If a schema's comments matter, edit that file by hand, or accept the loss and keep
the explanation in your docs instead.
:::

Because writes go through a temporary file and a rename, a multi-file save is
close to atomic. If a write does fail, the app keeps the last good in-memory state
and asks you to retry or reload — check `git status` before continuing.

Working in git makes all of this low-risk: save, then diff.

## Creating things

| Create | Where |
| --- | --- |
| Endpoint | Explorer **+** → New Endpoint, or palette |
| Module (resource) | Explorer **+** → New Module, creates `resources/<name>.yaml` |
| Actor | Explorer **+** → New Actor |
| Environment | Environment selector → New |

Names are validated as you type — no `.`, `/`, or `\`, since those break ids.
Nothing is written until you save, and a half-finished endpoint prompts before it's
discarded.

## Next

- [Running & debugging](/desktop/running/) — verify what you just edited
- [Authoring guide](/schema/authoring/) — the same concepts, in YAML
- [Common pitfalls](/schema/pitfalls/) — silent failures the app can't warn about
