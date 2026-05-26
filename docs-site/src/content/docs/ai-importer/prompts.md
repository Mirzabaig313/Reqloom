---
title: Multi-stage prompt suite
description: "Six prompts that turn API documentation into a runnable ChainAPI project. Read the playbook first."
---

The prompt suite splits AI schema generation into six stages, each with its own review gate. See the [AI importer playbook](/ai-importer/playbook/) for the full workflow.

The prompts themselves live at `prompts/import/` in the repository:

| Stage | Prompt file | Purpose |
|---|---|---|
| 1 | `01-discover.md` | Structured digest of the API |
| 2 | `02-plan-schema.md` | Written schema plan (human-reviewable) |
| 3 | `03-generate-actors.md` | YAML files for actors |
| 4 | `04-generate-resources.md` | YAML files for resources (one per call) |
| 5 | `05-generate-environment.md` | Environment file with placeholders |
| 6 | `06-fix-lint-errors.md` | Iterative fix-up |
