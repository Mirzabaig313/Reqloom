---
title: chainapi import
description: "Convert OpenAPI specs, Postman collections, Bruno files, Insomnia exports, or curl logs into a ChainAPI project."
---

```
chainapi import <file>
```

The direct importer (no LLM) supports:

- OpenAPI 3.x (YAML or JSON)
- Postman Collection v2.1+
- Bruno collections (folder of `.bru` files)
- Insomnia v4 exports

For Markdown docs and curl logs, use the [AI importer](/ai-importer/playbook/)
which leverages an LLM with multi-stage prompts.

Full content for this page is part of Phase 3 of the roadmap.
