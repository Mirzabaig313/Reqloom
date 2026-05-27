---
title: File structure
description: "Multi-file vs single-file project layouts, glob imports, and the two accepted YAML shapes."
---

ChainAPI projects are folders. Two structural styles are accepted:

- **Multi-file** (preferred for projects with > 5 resources) — separate files for actors, resources, environments, glued together by `imports:` in the project root `chainapi.yaml`.
- **Single-file** — everything inline. Good for very small projects or examples.

See the [authoring guide](/schema/authoring/) for examples of both shapes.
