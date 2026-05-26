---
title: chainapi run
description: "Execute an operation chain. Auto-resolves prerequisites, runs in topological order, prints HTTP status and timing per step."
---

```
chainapi run <resource.operation> [flags]
```

Flags:

| Flag | Purpose |
|---|---|
| `--project <path>` | Project root (default: cwd) |
| `--env <name>` | Environment file to load (default: `local`) |
| `--var key=value` | Override an env variable for this run |

The CLI prints a chain summary on completion. On failure, it shows the
HTTP status received and the first 200 chars of the response body for
the failing step.

Full content for this page is part of Phase 2 documentation.
