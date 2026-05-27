---
title: CLI overview
description: "The chainapi command-line interface: run, lint, import, dry-run, environments, and CI-friendly output formats."
---

The CLI is the daily-driver tool for ChainAPI. Three subcommands:

- [`chainapi run`](/cli/run/) — execute an operation chain
- [`chainapi lint`](/cli/lint/) — validate the schema
- [`chainapi import`](/cli/import/) — convert OpenAPI / Postman / Bruno / curl logs

Common flags that work across commands:

- `--project <path>` — path to the project root (defaults to cwd)
- `--env <name>` — select environment file (defaults to `local`)
- `--var key=value` — override env vars at run time
