# Reqloom Desktop: Design System

The visual and interaction spec for the Reqloom Qt 6.8 Widgets desktop app. This is the **Qt** design language: a token-driven `Theme` that emits a Qt Style Sheet at runtime, not web CSS. Source of truth for `desktop/src/theming/`, `desktop/src/views/`, and `desktop/src/widgets/`.

Register: **product** (a developer tool; design serves the task, not the other way around). Grounded in PRD §6.5 to §6.14 (UI requirements) and §7.5 (accessibility). Every token maps to a `Theme` field; every component maps to a class under `views/` or `widgets/`.

### How to read this document

This spec describes both the shipped UI and its intended direction. To keep it honest as the code evolves, each component and mechanism is tagged:

- **Shipped** — implemented in `desktop/src/` today. The described behavior matches the code.
- **Planned** — specified here, not yet built. Do not assume the class exists.
- **Partial** — exists but does not yet honor every rule below; the gap is called out inline.

When you change the code, update the matching tag in the same PR. A design doc that disagrees with the code is worse than no doc.

---

## 1. Design Principles

Reqloom is a workbench for backend engineers and QAs, not a consumer app. The design follows from that:

1. **Information density over whitespace.** Engineers want many operations, headers, and response fields visible at once. Spacing is tight and deliberate.
2. **The chain is the hero.** The dependency chain and run timeline are the product's reason to exist. They get the strongest visual treatment; everything else recedes.
3. **State is legible at a glance.** Session validity, cache state, run status, and null extractions are all encoded in a consistent color plus glyph vocabulary, so a user scanning the panel knows the system state without reading text.
4. **Keyboard first.** Per NFR-5.1, no mouse-only flows. Every interactive element has a focus state and a shortcut. The command palette (Cmd+P) is the primary navigation.
5. **Native, not branded.** Respect each OS (macOS menu bar, Windows 11 Fluent accents, Linux GTK hints). Reqloom does not impose custom chrome that fights the platform.
6. **Calm under load.** A running chain has a lot of motion potential. Restrain it: subtle progress, no scattered spinners. Motion communicates state change, nothing decorative.

### The product bar

The test is not "would someone say AI made this." Familiarity is a feature here. The test: would an engineer fluent in Linear, Raycast, or the JetBrains IDEs sit down and trust this, or pause at every subtly-off control? The tool should disappear into the task.

---

## 2. Color

Defined in `Theme.{h,cpp}` as OKLCH literals, resolved to sRGB `QColor`s in `Palette`, and emitted into a single application stylesheet by `Theme::styleSheet()`. There are no `.qss` files on disk; the stylesheet is built in C++ from the resolved palette and applied with `qApp->setStyleSheet()` (see §3, §13). All source values are **OKLCH**. Chroma drops as lightness approaches 0 or 100, so highlights and shadows never look garish.

### 2.1 Color strategy

**Restrained** (the product floor): tinted neutrals carry the surface; a single committed accent appears only on primary actions, current selection, focus, and state indicators. The accent never decorates. One surface may earn a heavier accent moment (the live run timeline), but the resting app is quiet.

This is the 60-30-10 rule read as visual weight, not pixel count: ~60% tinted-neutral surfaces, ~30% secondary text and borders, ~10% accent. The accent works because it is rare; spending it on decoration kills its signal.

### 2.2 Token layers

The shipped model is a single semantic layer. `Palette` holds the resolved semantic tokens below (`accentBase`, `surfaceRaised`, ...); `resolveLight()` / `resolveDark()` in `Theme.cpp` write the OKLCH literals straight into those fields. There is no separate primitive ramp object: the raw OKLCH values live inline at the two resolve sites, and a single accent-hue constant (`kAccentHue = 285`) ties the neutrals together.

That is deliberate for a palette this small. The rule that matters downstream is unchanged: **widgets and the stylesheet reference semantic names only** (`accent.base`, never a raw `oklch(...)` or hex). Light and dark differ solely in what the two resolve functions write.

If the palette grows to the point where the same OKLCH value is duplicated across many tokens, promote a primitive ramp then; until that pressure exists, the inline literals are simpler and the indirection would be ceremony.

### 2.3 The accent, and why it is not blue

The first-order category reflex is "developer API tool, therefore dark blue." The second-order trap is "API tool that avoids blue, therefore terminal green on black." Both are training-data defaults; both are rejected here.

The accent is a committed **indigo-violet** (`oklch hue 285`). It reads as considered rather than defaulted, it does not collide with any status hue, and it carries the product's "chain of linked steps" identity without leaning on the blue or terminal-green clichés. Every neutral is tinted toward this same hue at very low chroma (0.004 to 0.010), so the whole surface feels intentional rather than gray.

Never use pure `#000` or `#fff`. The darkest text and lightest surface are both tinted toward hue 285.

### 2.4 Semantic tokens

Reference these names in QSS, never raw values. `Theme` resolves them per active mode.

| Token | Role |
|---|---|
| `surface.base` | Window background |
| `surface.raised` | Panels, content surfaces (one level up from base) |
| `surface.sunken` | Input fields, code editor gutter |
| `surface.overlay` | Popovers, command palette, tooltips |
| `border.subtle` | Panel dividers, default control borders |
| `border.strong` | Focused control border, active selection |
| `text.primary` | Body text, values |
| `text.secondary` | Labels, captions, metadata |
| `text.disabled` | Inactive controls |
| `text.inverse` | Text on accent fills |
| `accent.base` | Primary action, focus ring, current selection |
| `accent.hover` | Accent hover state |
| `accent.muted` | Accent backgrounds (selected row tint) |

### 2.5 Status palette

These map directly to engine state enums (`StepResult::Status`, `ActorSession::State`, `ErrorClass`). The same hue means the same thing everywhere: explorer badges, timeline nodes, graph highlights. Each hue is distinct from the others and from the accent.

| Token | Hue | Meaning | Maps to |
|---|---|---|---|
| `status.idle` | neutral | Pending, not run | `StepResult::Status::Pending` |
| `status.running` | 230 (cyan) | In flight | `Running`, `Authenticating`, `Refreshing` |
| `status.success` | 150 (green) | 2xx, extraction resolved | `Succeeded`, session `Live` |
| `status.warning` | 75 (amber) | Retry, slow, null extraction | retry attempts, `null` extract result |
| `status.error` | 27 (red) | Failure | `Failed`, `ErrorClass` non-`Run` |
| `status.cancelled` | neutral | User cancelled | `Cancelled` |
| `status.blocked` | 320 (magenta) | Upstream failed, never ran | `Blocked` |
| `status.skipped` | faint neutral | Dry run, conditionally skipped | `Skipped` |

`status.warning` is reserved for the null-extraction highlight required by FR-9.11, a non-error condition that still demands attention. Do not reuse it for failures. `status.running` (cyan, hue 230) is deliberately separate from the accent (indigo-violet, hue 285) so "the app wants your attention" and "this step is executing" never blur together.

**Implementation note (shipped).** `StatusToken` is an eight-value enum (`Idle, Running, Success, Warning, Error, Cancelled, Blocked, Skipped`). `Theme::status(token)` returns the resolved hue; `Skipped` currently resolves to the same `statusIdle` color as `Idle`, since both read as "did not run" — only the glyph distinguishes them (§6.1). There is no separate `statusSkipped` field in `Palette`. If skipped ever needs its own hue, add the field rather than overloading idle.

**Status tints (shipped).** `Theme::statusTint(token)` returns an opaque background tint for a status, computed as a fixed ~16% mix of the status color toward `surface.raised` (not an alpha composite, per §2.9). This is what fills the background behind status pills and chips so they stay legible on any surface. It is computed, not stored, so it stays correct when the theme switches.

### 2.6 Light theme values

```
surface.base     oklch(0.985 0.003 285)   surface.raised   oklch(0.995 0.002 285)
surface.sunken   oklch(0.960 0.004 285)   surface.overlay  oklch(0.995 0.002 285)
border.subtle    oklch(0.900 0.005 285)   border.strong    oklch(0.780 0.008 285)
text.primary     oklch(0.240 0.010 285)   text.secondary   oklch(0.470 0.009 285)
text.disabled    oklch(0.680 0.006 285)   text.inverse     oklch(0.985 0.002 285)
accent.base      oklch(0.550 0.190 285)   accent.hover     oklch(0.480 0.200 285)
accent.muted     oklch(0.950 0.030 285)
status.running   oklch(0.600 0.140 230)   status.success   oklch(0.560 0.150 150)
status.warning   oklch(0.700 0.150 75)    status.error     oklch(0.560 0.200 27)
status.cancelled oklch(0.550 0.010 285)   status.blocked   oklch(0.560 0.150 320)
```

### 2.7 Dark theme values

```
surface.base     oklch(0.180 0.006 285)   surface.raised   oklch(0.220 0.007 285)
surface.sunken   oklch(0.140 0.006 285)   surface.overlay  oklch(0.260 0.008 285)
border.subtle    oklch(0.300 0.008 285)   border.strong    oklch(0.420 0.010 285)
text.primary     oklch(0.940 0.004 285)   text.secondary   oklch(0.680 0.006 285)
text.disabled    oklch(0.480 0.006 285)   text.inverse     oklch(0.180 0.006 285)
accent.base      oklch(0.680 0.170 285)   accent.hover     oklch(0.750 0.160 285)
accent.muted     oklch(0.300 0.060 285)
status.running   oklch(0.750 0.130 230)   status.success   oklch(0.780 0.160 150)
status.warning   oklch(0.800 0.150 75)    status.error     oklch(0.700 0.180 27)
status.cancelled oklch(0.680 0.010 285)   status.blocked   oklch(0.720 0.160 320)
```

Verify every text and background pair with a contrast tool before committing a QSS change. Dark-mode `status.*` and `accent.*` values are intentionally brighter to clear 4.5:1 on `surface.raised`. Qt does not parse OKLCH directly; `Theme` converts these to sRGB hex at load time, so the source of truth stays perceptual while the QSS receives values Qt understands.

### 2.8 Dark mode is not inverted light mode

Dark mode is a separate set of decisions, not a color swap:

- **Depth comes from surface lightness, not shadow.** The three-step `surface.sunken < base < raised < overlay` ramp climbs in lightness; dark mode drops the drop-shadows light mode uses.
- **Body text weight drops to 350** in dark (from 400). Light text on dark reads heavier than dark on light, so the lighter weight keeps the visual color even. *(Planned: `Theme::font` does not yet vary weight by appearance; see §4.3.)*
- **Accents desaturate slightly** relative to a naive invert, already reflected in the §2.7 values.
- **Placeholder text still needs 4.5:1.** The washed-out gray placeholder is the most common contrast failure; `text.secondary`, not `text.disabled`, is the floor for placeholders.

### 2.9 Alpha is a design smell

Heavy transparency means an incomplete palette and produces unpredictable contrast. Define explicit tint tokens instead of `rgba` overlays:

| Token | `Palette` field | Status | Built from | Use |
|---|---|---|---|---|
| `tint.cache` | `tintCache` | Shipped | accent at the row-tint lightness | cached explorer rows (FR-5.4) |
| `tint.substituted` | `tintSubstituted` | Shipped | accent at field-background lightness | resolved variable values (FR-6.2) |
| `tint.diffAdd` | `tintDiffAdd` | Shipped | success hue at low-saturation surface lightness | diff additions (FR-7.5) |
| `tint.diffRemove` | `tintDiffRemove` | Shipped | error hue at low-saturation surface lightness | diff removals (FR-7.5) |
| `tint.currentLine` | — | Planned | accent at gutter lightness | code-editor current line, lands with the CodeEditor (§6.8) |

Each shipped tint is a precomputed opaque OKLCH value resolved into `Palette`, not an alpha composite. `tint.currentLine` is intentionally absent from `Palette` today because the QScintilla `CodeEditor` it serves is not built yet (§6.8); add the field in the same change that introduces the editor. The only sanctioned alpha is the focus ring and the `status.running` pulse, where see-through is the point.

---

## 3. Theme: light, dark, and the default

Dark versus light is never a reflex. The scene that forces the answer:

> A backend engineer debugging a multi-role auth chain at their desk in mid-afternoon office light, alt-tabbing between this and their IDE in focused 45-minute sessions.

That scene does not force dark or light on its own. What it forces is this: the tool sits beside an IDE the developer already themed. The honest default is therefore **System**, following the OS appearance the developer already chose. Both themes are first-class peers, not one plus an afterthought, which also satisfies the dual-theme contrast requirement (NFR-5.3).

Primary craft target is dark, since the plurality of backend developers run dark IDEs, but every screen is designed and contrast-checked in both. The toggle offers Light, Dark, System, stored in `QSettings` under `appearance/mode`, applied live without restart.

**How "live" works (shipped).** `ThemeManager` resolves the effective appearance, then calls `qApp->setStyleSheet(theme.styleSheet())`; Qt re-applies the sheet across the whole widget tree, so no manual `unpolish`/`polish` walk is needed for stylesheet-driven properties. In System mode it connects to `QStyleHints::colorSchemeChanged` (Qt 6.5+) and re-applies when the OS appearance flips. Custom-painted atoms that cache palette-derived assets (e.g. `StatusBadge`) refresh on the `ThemeManager::themeChanged(const Theme&)` signal instead, since they paint outside QSS.

---

## 4. Typography

Per NFR-5.4, all sizes respect OS font scaling. Never hardcode pixel sizes that ignore `QApplication::font()`. Use `Theme` text styles, derived from the system base size.

### 4.1 Families

System fonts are correct for a product UI. They give native feel on every platform and they are what the user's eye already expects.

| Use | Font | Source |
|---|---|---|
| UI (labels, menus, body) | System default — always native | `QApplication::font()` |
| Code, JSON, headers, URLs | Bundled monospace (recommended), system fixed font today | `QFontDatabase::systemFont(QFontDatabase::FixedFont)`; see bundling note below |

**Shipped behavior.** `Theme::font(TextStyle)` derives every UI style from `QApplication::font()` so OS font scaling is respected (NFR-5.4), and the `Mono` style currently uses the platform fixed font (`QFontDatabase::systemFont(QFontDatabase::FixedFont)` — SF Mono on macOS, Consolas/Cascadia on Windows, the system monospace on Linux) with `QFont::Monospace` as the style hint. **No font is bundled today.**

**Recommended change: bundle one monospace for the code/data surfaces.** The UI font stays native (a bundled UI font fights the platform and is never worth it). The *monospace* is the opposite case, and the recommendation is to bundle a single high-quality open-source family (JetBrains Mono or Fira Code) for **all** platforms, not just Linux:

- The mono font is used only on surfaces that are inherently non-native to begin with: the JSON tree, the raw response view, the headers table, URLs, and the timeline's extracted values. A consistent code font there reads as deliberate, the way an IDE's editor font does, not as foreign chrome.
- Per-platform glyph metrics differ (SF Mono vs Cascadia vs DejaVu Sans Mono). Even with `tnum` keeping digits aligned, punctuation-dense JSON (`{`, `"`, `:`, `[`) advances differently across platforms, so tree indentation, wrapping, and the byte budgets in §4.2 shift between a developer's Mac and their Linux CI box. Bundling removes that variable: the same document lays out identically everywhere, which matters for a tool whose screenshots and bug reports cross machines.
- Cost is a one-time binary increase (JetBrains Mono ships a handful of weights at well under a megabyte each; bundle Regular and Bold only).

Mechanism if adopted:

- Add the `.ttf` files as Qt resources and register them in `Bootstrapper` with `QFontDatabase::addApplicationFont` before the first window shows.
- Point the `Mono` style at the bundled family by name, with the platform fixed font kept as the fallback in the `QFont` family list, so a failed resource load degrades to today's behavior rather than to a proportional font.
- License: JetBrains Mono and Fira Code are both SIL OFL 1.1. Bundling is permitted; include the license file in the app bundle and note it in `LICENSE` / third-party attributions per the OSS-scope rule in `AGENTS.md`.

Until this lands, the system monospace is the shipped fallback and the §4.2 metrics are validated against it. Track the bundling as the change that makes those metrics platform-independent.

### 4.2 Type scale

Product UIs hold many type elements, so the scale ratio is tight (roughly 1.15 between steps). Exaggerated contrast would read as noise, not hierarchy. Hierarchy comes from weight as much as size. Sizes are relative to the system base, scaled by ratio rather than fixed pixels.

| Style | Ratio | Weight | Use |
|---|---|---|---|
| `title` | 1.30 | 600 | Panel headers, dialog titles |
| `subtitle` | 1.15 | 600 | Section headers within a panel |
| `body` | 1.00 | 400 | Default text, values |
| `label` | 0.92 | 500 | Field labels, tab text |
| `caption` | 0.85 | 400 | Metadata, timestamps, evidence strings |
| `mono` | 0.92 | 400 | Code, JSON, headers, JSONPath |

Line height is 1.4 for body and mono, tighter (1.15 to 1.2) for `title` and `subtitle`. Prose blocks (help text, evidence strings) cap at 65 to 75ch. Data surfaces (response tree, headers table) run denser; a 120ch table is fine.

### 4.3 Type details

- **`tabular-nums` for all aligned numbers:** status codes, durations, byte counts, extraction values in the timeline. Without it, columns of numbers jitter as digit widths vary. Shipped via `QFont::setFeature("tnum", 1)` on the `Mono` style in `Theme::font`; apply the same to any new data style that renders aligned figures.
- **Letter-spacing only where it earns it:** method chips and any uppercase label get slightly open tracking (roughly +0.02em); everything else stays at the font default. Never track lowercase body text.
- **Weights are role-locked.** Today four roles map to Qt weights in `Theme::font`: body → `Normal` (400), label → `Medium` (500), title/subtitle → `DemiBold` (600), mono → the system fixed font's default. The same role uses the same weight everywhere; a 600 heading in one panel and a 500 heading in another is a bug. Dropping body to ~350 in dark mode (see §2.8) is a refinement not yet wired into `Theme::font` — when added, it belongs there, keyed on `appearance()`, not sprinkled across widgets.
- **Semantic style names, not sizes.** Widgets call `Theme::font(TextStyle::Title)` etc., never a raw point size.

---

## 5. Spacing & Layout

### 5.1 Spacing scale (px, pre-scaling)

Use the `Theme::space(n)` helper; never hardcode margins. As a workbench, default to the tighter end for intra-panel spacing and reserve the larger steps for section breaks. Vary spacing for rhythm; identical padding on every element reads as monotony.

| Token | px | Use |
|---|---|---|
| `space.xs` | 4 | Icon to text, badge padding |
| `space.sm` | 8 | Control padding, list row vertical |
| `space.md` | 12 | Field gaps, panel inner margin |
| `space.lg` | 16 | Section gaps |
| `space.xl` | 24 | Panel to panel (rare; panels usually share a splitter) |
| `space.xxl` | 32 | Empty-state centering only |

### 5.2 The three-pane workbench

```
+--------------+--------------------------+------------------+
| Project      |  Request Editor          |  Response Viewer |
| Explorer     |  (resolved request +     |  (JSON tree /    |
| (actors +    |   chain preview)         |   raw / headers  |
|  resources   |                          |   / diff)        |
|  tree)       +--------------------------+------------------+
|              |  Timeline (executed chain, per-step r/r)    |
+--------------+---------------------------------------------+
```

- Left, center, and right via a horizontal `QSplitter`. The timeline sits in a nested vertical `QSplitter` under center and right.
- Splitter handles use `border.subtle`, 1px, with a 4px hit target.
- Default ratio 22 / 44 / 34. Persist the user's splitter sizes to `QSettings`.
- Minimum panel widths prevent collapse to unusable. A panel hides entirely via the View menu, not by dragging to zero.

Familiar structure is an affordance here. Top-bar plus side-nav plus tabs are expected patterns; do not reinvent them for flavor.

### 5.3 Density modes

Two modes via the View menu, stored in `QSettings`:

- **Comfortable** (default): list rows at `space.sm` vertical padding.
- **Compact**: list rows at `space.xs`, for users with 500+ operations who want maximum visible.

---

## 6. Components

Each maps to a class under `views/` (composite panels) or `widgets/` (reusable atoms). Every interactive component ships with all of: default, hover, focus, active, disabled, loading, error. Shipping half of these is shipping a bug.

### 6.1 StatusBadge (`widgets/StatusBadge`) — Shipped

The most-used atom. A small pill or dot encoding a `status.*` value.

- **Never color alone.** Pair each status with a glyph: check for success, filled dot for running, cross for error, slashed circle for cancelled, pause for blocked, triangle for warning or null. Color-blind users must distinguish states (the strongest accessibility rule in this app).
- Two variants: `Dot` (8px, for tree rows) and `Pill` (text plus glyph, for timeline and headers).
- `status.running` carries a subtle 1.2s opacity pulse, the one ambient animation in the app. It stops the instant state changes.
- The glyph vocabulary is exposed as a static `StatusBadge::glyph(token)` so trees and tables that can't host a child widget render the *same* glyphs through a delegate, keeping the vocabulary consistent across widget and non-widget surfaces.

### 6.2 Project Explorer (`views/ProjectExplorerWidget`), FR-5 — Shipped

- `QTreeView` with a custom delegate. Two top-level groups: Actors (each with a session `StatusBadge`) and Resources (each expands to operations with method chips).
- Method chips: colored mono text on `surface.sunken`, uppercase. Coloured by the dedicated **method vocabulary** (§6.2a), not the status palette. Muted, not loud.
- **Cache state (FR-5.4):** a cached row carries a faint full-width `tint.cache` row fill plus a small cache glyph in the trailing column. (No side accent border; a colored left stripe is a banned pattern and reads as decoration rather than meaning.)
- Search field (FR-5.2) pinned to the top, filters as you type. Cmd+F focuses it.
- Right-click menu (FR-5.3): Run, Run with Override, Edit Schema, View Dependencies.

### 6.2a HTTP method colour vocabulary (`MethodColor`) — Shipped

HTTP verbs get a **dedicated colour vocabulary**, deliberately separate from the status palette (§2.5) so a method chip's colour never reads as a run state (a green `GET` must not be mistaken for a succeeded step). Developers recognise request types at a glance — the convention most API tools share:

| Method | Hue | Mnemonic |
|---|---|---|
| `GET` | 255 (blue) | safe read |
| `POST` | 150 (green) | create |
| `PUT` | 55 (orange) | replace |
| `PATCH` | 95 (yellow) | partial update |
| `DELETE` | 27 (red) | destructive |
| `HEAD` / `OPTIONS` / other | — | neutral (`text.secondary`) |

Resolved in `Palette` as `methodGet/methodPost/methodPut/methodPatch/methodDelete`; accessed via `Theme::method(MethodColor)` and the opaque `Theme::methodTint(MethodColor)` (same surface-mix technique as `statusTint`, §2.9 — no alpha). `format::methodColor(verb)` maps a verb string to the token. Used by the explorer chips, the request address-bar method pill (`#methodPill[methodClass="..."]`), and the execution-chain nodes (§6.3), so the vocabulary is identical across every surface. Dark-mode values are brightened to clear AA on `surface.raised`. The hues are chosen distinct from the accent (285) and from every status hue.

### 6.3 Request Editor (`views/RequestEditorPanel`), FR-6 — Shipped

- **Read-only by default** (FR-6.1). The panel reads as a preview, not a form: `surface.sunken` fields, no edit affordance until Override Mode.
- Override Mode toggle in the panel header. When on, fields become editable with a `border.strong` outline and a persistent "Override active, one-shot" banner tinted `status.warning`. Headers, query params, and form-data rows use the shared `widgets/KeyValueEditor` (single-click-editable rows; a `FileCapable` mode adds a per-row file picker that writes the curl-style `@/path` reference the engine's multipart builder expects).
- Resolved-request view (FR-6.2): shows the request after variable substitution. Substituted values get a subtle `tint.substituted` background so the user sees what came from where.
- Chain preview (FR-6.3): the **execution chain** rendered visually by `widgets/ChainView` — a vertical sequence of nodes, each a method-coloured pill (§6.2a) plus operation id, with `↓` connectors between steps and the invoked operation marked as the `target`. This is the product's hero surface (§1.2): the chain is shown, not spelled out as a label. When an operation has no declared dependencies, the view shows a single muted hint to run a Dry Run for the full resolved chain.
- Two buttons: **Send** (`accent.base`, primary) and **Send Cleanly** (secondary, `border.strong` outline). Cmd+Enter sends; Cmd+Shift+Enter sends cleanly.

### 6.4 Response Viewer (`views/ResponseViewerPanel`), FR-7 — Shipped

- Tabbed: Tree, Raw, Headers, Diff (a `QTabWidget`).
- Tree (FR-7.1): a `QTreeWidget` populated from the parsed JSON, monospace. Clicking a value copies its JSONPath (FR-7.4) and emits `jsonPathCopied`, which the shell turns into a confirmation `Toast`.
- Raw (FR-7.2): a read-only `QPlainTextEdit` with pretty-printed JSON. (The richer QScintilla `CodeEditor` with syntax highlighting in §6.8 is **planned**; the raw tab uses a plain text view today.)
- Headers: masked header view — `Authorization`, `Cookie`, and other sensitive headers are redacted before display, mirroring the engine's redaction-first contract.
- Diff (FR-7.5): inline line diff via `widgets/diff::lineDiff` (LCS over lines), additions filled `tint.diffAdd`, removals filled `tint.diffRemove`, both light enough that text stays readable. History is cleared on project switch (`clearHistory`) so diffs never compare across projects.
- Status line at top: status code in the matching `status.*`, duration in `caption`. The last status is cached so a runtime theme switch can re-resolve the label color.
- **Capture-off state.** Bodies only arrive when the run opted into body capture (`RunController::setCaptureResponseBodies(true)`). When capture is off, the body tabs show a placeholder explaining why they're empty rather than a blank pane (§10 Empty).

### 6.5 Timeline (`views/TimelinePanel`), FR-7.6 — Shipped

- A horizontal sequence of step nodes, each a `StatusBadge.Pill` plus operation name: the executed chain, left to right.
- Click a node to load that step's request and response in the viewer above.
- **Extraction values (FR-9.11):** below each node, show extracted values. `null` results highlight in `status.warning` with a connector drawn to the downstream consumers that depended on them. This is the timeline's most important job; make it prominent.
- During a run, the active node pulses `status.running`; completed nodes settle to their terminal status. This is the one surface allowed a heavier accent moment.

### 6.6 Dependency Graph (`views/DependencyGraphView`, Phase 2), FR-8 — Planned

This view is **not yet built**; no `views/DependencyGraphView` or `qml/` directory exists in `desktop/` today. The spec below is the intended design for the Phase 2 work.

- A `QQuickWidget` hosting a QML graph (the one place QML appears; see the planned `qml/DependencyGraph.qml`).
- Nodes use the same `status.*` palette. The execution path highlights from `status.running` to the terminal color (FR-8.3).
- Circular dependencies (FR-8.4) draw with `status.error` edges and a warning banner.

### 6.7 Command Palette (`widgets/CommandPalette`), FR-14 — Shipped

- Cmd/Ctrl+P. `surface.overlay`, centered, 600px max width, soft shadow.
- Fuzzy find over operations via `widgets/FuzzyMatch`; recent at the top (FR-14.3). A `>` prefix switches to global commands (FR-14.4).
- Keyboard only: arrows navigate, Enter runs, Esc closes. The selected row uses `accent.muted`.

### 6.8 CodeEditor (`views/CodeEditor`) — Planned

**Not yet built.** The current Raw response tab (§6.4) is a read-only `QPlainTextEdit`; YAML/JS schema editing has no dedicated editor yet. When built:

- A QScintilla wrapper for YAML, JSON, and JS hook editing. The syntax theme derives from `Theme`; no QScintilla default colors.
- Gutter on `surface.sunken`. Current line filled `tint.currentLine` (add that token to `Palette` in the same change, see §2.9). Matches the `mono` type style.

### 6.9 Supporting atoms — Shipped

Smaller pieces the panels above compose from, each token-driven and theme-reactive:

- **`widgets/PanelHeader`** — the consistent header strip (a `subtitle`-style title plus an optional trailing actions area) that gives the explorer, request, response, and timeline panels the same rhythm (§5).
- **`widgets/Toast`** — the transient, self-deleting, click-through confirmation overlay near the bottom-center of its parent. Backs "copied JSONPath" (FR-7.4) and the undo pattern (§11.4).
- **`widgets/EmptyState`** — the centered title + explanation + optional primary-action panel used for first-run and no-data surfaces (§10 Empty, PRD §12).
- **`widgets/KeyValueEditor`** — the Postman-style key/value row list used by Override Mode (§6.3).
- **`views/SecretsDialog`** — lists the secrets a project references via `{{secret.NAME}}` and whether each is present in the OS keychain. **Shows presence only, never stored values**, so secrets can't leak through the UI (PRD §13.3, FR-11.3/11.4).
- **`widgets/FuzzyMatch`**, **`widgets/diff::lineDiff`**, **`views/Formatting`** — pure, unit-tested helpers (fuzzy scoring, line diff, value formatting) with no Qt-widget dependency.

---

## 7. Cognitive Load

A workbench shows a lot at once. The risk is not blankness, it is overload. The job is to spend the user's working memory only on the task (the chain), never on decoding the interface. Working memory holds about four items at once (Miller, revised by Cowan); design every decision point to stay under that.

### 7.1 The three loads

- **Intrinsic** (the API testing task itself): cannot be removed, only structured. Group an operation with its actor, its dependencies, and its extractions so the user reasons about one chain at a time.
- **Extraneous** (bad design): eliminate ruthlessly. Inconsistent badges, mystery icons, a Send button that looks different in two panels. Pure waste.
- **Germane** (learning the tool): support it. Consistent patterns reward the user for learning once, then applying everywhere.

### 7.2 Working-memory budget per surface

| Surface | Keep under |
|---|---|
| Explorer top-level groups | 2 (Actors, Resources). Everything else nests. |
| Primary actions per panel | 1 primary (`accent.base`) + at most 2 secondary; the rest go in the context menu or palette |
| Request Editor sections visible at once | 4 before a visual break (method+URL, headers, body, chain preview) |
| Chain-preview steps shown expanded | collapse by default; expand on demand |
| Tabs in the Response Viewer | 4 (Tree, Raw, Headers, Diff) |

When a surface needs more, **group or progressively disclose**. Never show a wall of options.

### 7.3 Progressive disclosure

- Request Editor is read-only until Override Mode is requested. The edit affordances do not exist until needed.
- Chain preview is collapsed; the user expands it when they want to see the steps.
- Per-field AI evidence strings (FR-9.9) are hidden behind a disclosure on each imported field, not shown inline by default.
- Advanced run options (force re-run scope, retry override) live in a context menu, not the main panel.

### 7.4 Co-locate, never make the user remember

The Memory Bridge is the worst load for this tool: forcing the user to remember a value from one panel to act in another. Counter it:

- The resolved request (FR-6.2) shows substituted values in place, so the user never holds "what did `{{auth.token}}` resolve to" in their head.
- The timeline shows each step's extracted values with connectors to downstream consumers (FR-9.11), so the data flow is on screen, not in memory.
- Clicking a timeline node loads that step's full request and response in the viewer above, co-locating everything needed to judge one step.

### 7.5 The squint test

Blur the screen (or screenshot and blur). The running step, the failed step, and the primary action must still be identifiable. If every panel reads at the same visual weight when blurred, the hierarchy has failed. Fix with weight, color, and space together, never size alone.

---

## 8. Iconography

- One consistent icon set, **bundled for all platforms** (Lucide or Phosphor), rendered through `QIcon` / `QSvgRenderer` so it is pixel-identical at any DPI on every OS (see §14.1 item 5; this is why there is no per-OS icon split).
- 16px default, 20px for the toolbar, scaled with OS font scaling.
- Icons are `text.secondary` by default, `text.primary` on hover or active, `accent.base` when representing the active primary action.
- Status glyphs (§6.1) are the exception; they carry their `status.*` color and never appear without it.
- Geometrically centered glyphs often look off-center; nudge play and arrow glyphs toward their direction so they read as centered (optical, not geometric, alignment).

---

## 9. Motion

Minimal and functional, in keeping with principle 6. Product motion conveys state, never decoration, and stays in the 120 to 250ms range so users in flow never wait on choreography.

| Element | Motion | Duration |
|---|---|---|
| `status.running` badge | opacity pulse 1.0 to 0.55 and back | 1.2s loop |
| Panel show or hide | width and opacity ease | 150ms |
| Command palette | fade plus 4px rise | 120ms |
| Toast (copied JSONPath) | fade in and out | 100ms in, 1.5s hold, 200ms out |
| Tab switch | none (instant) | n/a |
| Tree expand or collapse | native Qt default | n/a |

Ease out with an exponential curve (ease-out-quart or quint). No bounce, no elastic, no spring. No spinners on individual rows; a running chain shows progress through the timeline, not scattered loaders. No orchestrated load sequence; the app loads into a task. If the OS requests reduced motion, disable the pulse and all transitions.

### 9.1 Pulse coordination (one tick, not one timer per badge)

**Shipped today:** each `StatusBadge` in the `Running` state owns its own `QVariantAnimation` (1.2s loop, `InOutSine`, `valueChanged` → `update()`). That is correct and cheap while only one or two steps run at a time, which is the MVP's single-threaded execution model.

**The scaling hazard:** the timeline can hold many rows, and a future graph view (§6.6) many nodes. If every running node drives an independent animation, their `update()` calls fire on uncorrelated frames, so the compositor repaints on nearly every tick of every badge instead of once per frame. The cost is repaint count, not the animation math.

**The rule for more than ~3 concurrent pulses:** drive all pulsing elements from a **single shared ticker**, not N animations. One `QTimer` (or one `QVariantAnimation`) at roughly 60fps owns the phase; each badge reads the shared opacity in `paintEvent` and is told to repaint together. Because every badge reads the same phase, the pulse also stays visually in sync rather than drifting. Sketch:

- A small `PulseClock` singleton-per-window owns one looping driver and exposes the current opacity plus a `tick()` signal.
- A badge entering `Running` subscribes (connects `tick` to its `update`); leaving `Running` unsubscribes. When the subscriber count hits zero, the clock stops, so an idle app burns no timer.
- Batch the repaint: prefer invalidating the smallest region (the badge rects), and let Qt coalesce them into one frame.

Keep the current per-widget animation until a surface actually shows several simultaneous pulses; introduce `PulseClock` as part of the work that creates that surface (the live timeline strip or the graph), not speculatively. Honor reduced-motion in one place: the clock simply never starts, and badges paint at full opacity.

---

## 10. States Every Component Must Handle

Define all of these for each interactive widget:

- **Default:** resting.
- **Hover:** `text.primary` or `accent.hover`, pointer cursor.
- **Focus:** `border.strong` ring, keyboard reachable. Never remove focus outlines (NFR-5.1).
- **Active or pressed:** momentary darken.
- **Selected:** `accent.muted` background.
- **Disabled:** `text.disabled`, no pointer events, a `caption`-level explanation when non-obvious. No heavy or full-saturation color on inactive states.
- **Loading:** for async-bound widgets, a skeleton state, not a spinner in the middle of content, and never a blocking modal spinner.
- **Empty:** first-run and no-data states get a centered message plus a primary action ("Import an API", "Open a project"), and they teach the interface rather than saying "nothing here." See PRD §12.
- **Error:** inline, `status.error`, with the `ErrorCode` string (`toCodeString`) and a human message. Never a raw stack trace.

Hover and focus are different states for different users. Keyboard users never see hover. Design both; never ship one as a stand-in for the other.

---

## 11. Interaction & Keyboard

Per NFR-5.1 the app is fully keyboard-operable. These patterns translate the web focus and overlay rules into Qt terms.

### 11.1 Focus, not hover, is the contract

- Every interactive widget has a visible focus ring: `border.strong`, 2px, drawn outside the control, 3:1 against its surroundings. Qt's `:focus` pseudo-state in QSS, never suppressed.
- Mouse hover may add a subtle shift, but focus is the state that must always be visible, because keyboard and screen-reader users live in it.
- Tab order follows reading order. Set it explicitly with `setTabOrder` where the widget tree does not already match.

### 11.2 Roving focus inside groups

For the explorer tree, the timeline node strip, and the response tabs, use Qt's native group navigation: arrow keys move within the group, Tab leaves it. Do not make every node a separate tab stop, that turns a 50-step chain into 50 tabs.

### 11.3 Overlays and stacking

Command palette, context menus, and tooltips are top-level popups (`Qt::Popup` / `QMenu` / `QToolTip`), so they escape panel clipping and stack correctly without manual z-fighting. Define a semantic stacking order and keep to it:

```
panel  <  splitter handle  <  dropdown/menu  <  command palette  <  toast  <  tooltip
```

Light-dismiss (click outside closes, Esc closes) is mandatory for the palette and menus. Esc also cancels a running chain per PRD §9.3, so palette-open Esc closes the palette first, then a second Esc reaches the run.

### 11.4 Destructive actions: undo over confirm

Prefer an undo toast to a confirmation dialog. "Reset Cache", "Delete Environment", and similar remove immediately and show a 5s undo toast; the real delete happens when the toast expires. Reserve a blocking confirm only for the truly irreversible (deleting a project file on disk). Modals are the last resort, not the first thought (per the shared bans).

### 11.5 Touch and hit targets

Even on desktop, interactive targets are at least 28px tall (Qt desktop convention; the 44px web touch minimum relaxes for mouse-driven density, but icon-only buttons get padding to a comfortable click target). Visual size and hit target are separate; a 16px icon button still claims a larger clickable area.

---

## 12. Accessibility Checklist (NFR-5)

Every PR touching `desktop/` verifies:

- [ ] All controls reachable and operable by keyboard (NFR-5.1). Tab order is logical.
- [ ] Every control has an accessible name via `setAccessibleName` and `setAccessibleDescription` (NFR-5.2).
- [ ] Text and background pairs meet WCAG 2.1 AA in both themes (NFR-5.3), verified with a contrast tool, not by eye.
- [ ] No information conveyed by color alone; status always pairs color with a glyph (§6.1).
- [ ] Placeholder text meets 4.5:1 (use `text.secondary`, never `text.disabled`).
- [ ] Layout survives OS font scaling to 200% without clipping (NFR-5.4).
- [ ] Focus indicators visible and never suppressed.
- [ ] Hover and focus designed separately; keyboard users get a focus state on every control.
- [ ] Screen reader announces run start and finish without requiring focus movement (see §12.1).

### 12.1 Announcing state changes to a screen reader

A status change the user can see (a badge flipping to `Running`, then `Succeeded` or `Failed`) is invisible to a screen reader unless the app actively tells the accessibility layer. Repainting a widget does not notify the OS; neither does calling `setAccessibleName` after construction on its own. The run can start, stream, and finish while focus sits in the explorer, so we cannot rely on a focus change to carry the news.

Push the change explicitly via `QAccessible`:

- **State transitions** (a step or the whole run changing status): construct a `QAccessibleStateChangeEvent` for the widget and post it with `QAccessible::updateAccessibility(&event)`. This is the right event for "this thing's state changed in place."
- **A new row appearing** (a step row added to the timeline as it begins): a `QAccessibleEvent` with `QAccessible::ObjectShow` on the new row so the reader knows a child arrived.
- **Run start / finish as a discrete announcement** (not tied to any one widget): post a `QAccessibleAnnouncementEvent` (Qt 6.8) carrying a short message such as "Run started, 7 steps" and "Run finished: 6 succeeded, 1 failed." This is the cleanest path for a transient, focus-independent announcement and is available on the project's pinned Qt 6.8 baseline.

Rules:

- Keep announcements terse and factual; they are read aloud serially and compete with the user's flow. One sentence, the outcome first.
- Fire from the same slot that updates the visual state (the `RunController` event handlers the timeline already listens to in §6.5), so visual and audible state never diverge.
- Do not announce every streamed event. Announce run start, run finish, and per-step *failures*; routine per-step success is visible on the timeline and would be noise if spoken.
- Respect reduced-motion only for animation, never for announcements: a user who suppressed the pulse still needs to hear that the run finished.

*(Status: planned. `StatusBadge::setStatus` updates `setAccessibleName`, but no `QAccessible::updateAccessibility` / announcement calls exist yet; add them in the `RunController`-to-timeline wiring.)*

---

## 13. Implementation Notes

- **Tokens, not raw values.** All color, spacing, and type live in `Theme`. The stylesheet is generated by `Theme::styleSheet()` from the resolved `Palette`; custom-painted widgets read `theme.palette()` / `theme.status()` directly. A raw hex or OKLCH literal anywhere outside `Theme.cpp`'s two resolve functions is a review failure.
- **One semantic token layer (§2.2).** `Palette` holds resolved semantic colors; the OKLCH literals live inline in `resolveLight()` / `resolveDark()`. There is no separate primitive-ramp object yet; promote one only when duplication demands it. Widgets touch semantic names exclusively.
- **Tint tokens, not alpha.** The `tint.*` tokens (§2.9) are precomputed opaque values; `statusTint()` likewise computes an opaque mix, not an alpha composite. Do not reintroduce `rgba`/alpha composites for row highlights, diffs, substituted values, or status fills.
- **OKLCH at the source, sRGB at the edge.** `Color::oklch` converts perceptual values to sRGB `QColor` once at resolve time, clamping out-of-gamut results per channel, since Qt's QSS parser does not read OKLCH.
- **Status colors live on `Theme`, not `QPalette`.** Tokens `QPalette` can't express (the status vocabulary, spacing scale, type scale) are methods on `Theme` (`status`, `statusTint`, `space`, `font`). `Theme` is value-resolved per appearance, not a global singleton; the active one is owned by `ThemeManager` and handed to widgets via `setTheme(...)` and the `themeChanged` signal. *(There is no separate `ThemeExtension` type; that earlier name is retired.)*
- **Theme toggle.** Light, Dark, System, stored in `QSettings` (`appearance/mode`), applied live without restart via `qApp->setStyleSheet(theme.styleSheet())`. Custom-painted atoms refresh on `themeChanged` rather than a manual `unpolish`/`polish` walk (§3).
- **No inline styles.** Per-widget `setStyleSheet` is banned except for genuinely one-off cases; prefer object-name and dynamic-property selectors in the central sheet built by `Theme::styleSheet()` (e.g. `QPushButton#primaryAction`, `QWidget[density="compact"]`).
- **Theme and density are runtime, not compile-time.** Both switch live.
- **Semantic stacking order**, not arbitrary `raise()` calls: `panel < splitter handle < dropdown/menu < command palette < toast < tooltip` (per §11.3). Top-level popups handle this naturally; do not hand-tune stacking with magic values.
- **Elevation is subtle.** In light mode, a shadow you can clearly see is too strong; use the smallest elevation that separates the surface. In dark mode, separate by surface lightness (§2.8), not shadow.
- **`gap`/layout spacing over per-widget margins** where Qt layouts allow it, so spacing stays consistent and centralized.

---

## 14. Qt Leverage (What to Reach For, and What to Avoid)

The UI is built on Qt's **item-widget** convenience classes (`QTreeWidget`, `QTableWidget`, `QListWidget`), not the model/view stack. That choice decides which Qt capabilities pay off and which fight the current code. This section records the decisions so contributors don't relitigate them or reach for the wrong tool when chasing "premium feel."

The throughline: the real UI/UX leverage is **theme-reactive custom painting via `QStyledItemDelegate`**, which turns the `Theme` tokens (§2, §4, §5) into the §6 component vocabulary on the trees that already exist. The rest is either already specced (graph, a11y, icons) or a trap §2.9 / §9 already reject.

### 14.1 Take these (high leverage, low conflict)

1. **`QStyledItemDelegate` on the existing trees — the biggest unused lever.** A custom delegate works on `QTreeWidget` exactly as on `QTreeView`, so no model/view migration is needed to gain it. `paint()` is the correct home for the §6 vocabulary that per-item `setForeground`/`setIcon` cannot express: the §6.1 status glyph + color, the §6.2 method chips and full-width `tint.cache` row fill (not a banned side stripe), and the §6.5 extraction values in success-green / null-warning. One delegate per tree (explorer, timeline, response) makes the whole §6 surface theme-reactive in the place Qt intends. This is the change that moves the UI from "styled widgets" to "designed tool." Start with a `StatusDelegate` on the timeline (the §1 hero surface).

2. **`QSortFilterProxyModel` — only paired with a model/view migration, and only when scale demands it.** The explorer's filter (FR-5.2) is hand-rolled `setHidden` looping today: fine for hundreds of operations, and it cannot rank by relevance. Promote the explorer to `QAbstractItemModel` + `QTreeView` + a proxy (reusing `widgets/FuzzyMatch` for scoring) only when the 500+ operation case in §5.3 is actually hit. Below that, the migration is premature; the current `QTreeWidget` stays.

3. **`QKeySequence::StandardKey` for shortcuts.** §1 principle 5 is "native, not branded" and §11 is keyboard-first. Define actions in `buildShortcuts()` with standard keys (`Find`, `Cancel`, etc.) so Cmd vs Ctrl resolves per-OS automatically. Free cross-platform correctness.

4. **`QAccessible` events for state changes (already specced in §12.1, still unbuilt).** Nothing currently fires `QAccessibleAnnouncementEvent` for run start/finish or `QAccessibleStateChangeEvent` for step transitions. This is the highest-value outstanding accessibility work and the only way the focus-independent announcements §12 commits to actually reach a screen reader.

5. **Bundled SVG icons via `QIcon` / `QSvgRenderer`.** Same logic as the bundled monospace in §4.1: bundle one set (Lucide or Phosphor) for all platforms and render losslessly at any DPI, so the toolbar is pixel-identical on 1080p and 4K across the three OSes. This supersedes the SF-Symbols-on-macOS hedge in §8 — bundle everywhere, drop the per-OS split.

### 14.2 Right tool, but Phase 2 only

6. **`QQuickWidget` for the dependency graph (§6.6).** The Quick scene graph is the only genuinely GPU-accelerated path in Qt, so node dragging, bezier edges, and zoom belong there rather than in a hand-built `QGraphicsScene`. Hold the architectural boundary: the QML scene consumes engine state through `Events.h` / `PublicApi.h` only, and Quick stays desktop-only so the engine boundary guard keeps passing (AGENTS.md). This is the one surface where QML earns its weight.

### 14.3 Do not reach for these

- **`QGraphicsBlurEffect` / glassmorphism on the palette or menus.** Banned by §2.9 (alpha is a smell) and the §15 anti-reflex notes. Independently, on the QWidget path it is CPU-rasterized into an offscreen pixmap and janks on a repainted surface. The palette stays an opaque `surface.overlay` with a 1px border and a restrained shadow.
- **`QGraphicsOpacityEffect` for the running pulse.** Forces offscreen rendering and is not hardware-accelerated. The §9.1 shared-tick approach (every badge reads one shared phase and repaints together) is both the correct design and cheaper.
- **`QPropertyAnimation` as a "premium" swap for the current pulse.** For a single `StatusBadge` it costs the same as the `QVariantAnimation` already in the code; neither is GPU-accelerated in the widget world. What scales is repaint coordination (§9.1), not the animation class. No change warranted.

---

## 15. Anti-Reflex Notes (Keep This Tool From Looking Defaulted)

A short list so future contributors do not drift back to category defaults:

- The accent stays indigo-violet (hue 285). Do not "simplify" it back to SaaS blue.
- `status.running` stays cyan (hue 230), distinct from the accent. Do not merge them.
- Neutrals stay tinted toward 285. Do not flatten to pure gray, and never use `#000` or `#fff`.
- No colored side stripes on rows, cards, or alerts. Use full borders, background tints, leading glyphs, or nothing.
- No gradient text, no decorative glassmorphism, no hero-metric template.
- Cards are the lazy answer; this app is mostly panels and trees, so reach for a card only when it is genuinely the right affordance, and never nest them.

---

## 16. Reference

- PRD §6.5 to §6.14 (UI requirements), §7.5 (accessibility), §12 (first-run).
- Project Layout §1 (`desktop/` tree), §2.5 (desktop CMake target).
- `AGENTS.md`: use `/qt-reviewer` for any change here; `/qt-patterns` skill for Qt 6.8 idioms.
- Engine state enums this design maps to: `engine/include/reqloom/engine/RunContext.h` (`StepResult::Status`, `ActorSession::State`), `ErrorCodes.h` (`ErrorClass`).

### Source map (where each section lives)

| Spec section | Implementation |
|---|---|
| §2 Color, tints, §4 Type, §5.1 Spacing | `theming/Theme.{h,cpp}`, `theming/Color.{h,cpp}` |
| §3 Theme toggle, System mode | `theming/ThemeManager.{h,cpp}` |
| §6.1 StatusBadge | `widgets/StatusBadge.{h,cpp}` |
| §6.2 Project Explorer | `views/ProjectExplorerWidget.{h,cpp}` |
| §6.3 Request Editor | `views/RequestEditorPanel.{h,cpp}`, `widgets/KeyValueEditor.{h,cpp}` |
| §6.4 Response Viewer | `views/ResponseViewerPanel.{h,cpp}`, `widgets/LineDiff.{h,cpp}`, `views/Formatting.{h,cpp}` |
| §6.5 Timeline | `views/TimelinePanel.{h,cpp}` |
| §6.6 Dependency Graph | *planned — not in tree* |
| §6.7 Command Palette | `widgets/CommandPalette.{h,cpp}`, `widgets/FuzzyMatch.{h,cpp}` |
| §6.8 CodeEditor | *planned — not in tree* |
| §6.9 Atoms | `widgets/PanelHeader`, `widgets/Toast`, `widgets/EmptyState`, `views/SecretsDialog` |
| Shell, settings, run wiring | `views/MainWindow`, `application/*` (`Bootstrapper`, `ProjectModel`, `RunController`, `LayoutSettings`, `EnvironmentSettings`, `SecretManager`) |
