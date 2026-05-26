---
title: chainapi lint
description: "Validate the schema and dependency graph without making any HTTP requests."
---

```
chainapi lint [--project <path>]
```

Validates:

- YAML syntax of every project file
- Schema version compatibility
- All `{{X.y}}` references trace to a real producer
- Dependency graph has no cycles
- Every operation's chain resolves cleanly

Exits 0 on success, non-zero on any error. Use in pre-commit hooks and
CI pipelines.
