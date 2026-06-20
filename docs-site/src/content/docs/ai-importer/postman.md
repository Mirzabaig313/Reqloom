---
title: Importing from Postman
description: "Use Reqloom's direct Postman importer. Faster and more reliable than going through an LLM."
---

Postman collections (v2.1+) import directly without an LLM:

```bash
reqloom import postman-collection.json
```

The importer:

- Maps Postman folders to resources (best-effort, with manual override)
- Maps Postman environments to Reqloom environments
- Converts pre-request and test scripts to Reqloom hooks (with warnings for unsupported APIs)
- Detects token-extraction patterns and converts them to declarative `extract:` blocks

Plan to spend 15-30 min reviewing the output for any non-trivial collection.
