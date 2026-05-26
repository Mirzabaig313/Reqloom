---
title: Importing from OpenAPI
description: "Use the AI importer with OpenAPI 3.x specs. Direct parser available for non-LLM imports."
---

Two paths for OpenAPI input:

1. **Direct parser (no LLM)** — `chainapi import openapi.yaml`. Faster, deterministic, free.
2. **AI importer with prompts** — better for OpenAPI specs that lack rich descriptions or have unusual auth flows.

Use the direct parser first; fall back to the AI importer if the result needs significant editing.

See [AI importer playbook](/ai-importer/playbook/) for the prompt workflow.
