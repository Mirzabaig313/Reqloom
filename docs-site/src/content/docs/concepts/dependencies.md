---
title: Dependency resolution
description: "How ChainAPI builds the prerequisite chain for any target operation: implicit edges from variable references plus explicit depends_on declarations."
---

When you ask to run an operation, the engine resolves the dependency chain by combining:

1. **Implicit edges** — any `{{X.y}}` reference in a path/body/header implies a dep on whichever operation produces `X.y`
2. **Explicit edges** — `depends_on:` declarations for prerequisites that don't produce values
3. **Topological sort** with deterministic lexicographic tie-break

See [Mental model](/concepts/mental-model/) for the algorithmic overview and [Engine requirement](/reference/engine-requirement/) §3.1 for the full spec.
