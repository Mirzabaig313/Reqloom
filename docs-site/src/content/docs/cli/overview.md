---
title: CLI overview
description: "The reqloom command-line interface: run, lint, import, dry-run, environments, and CI-friendly output formats."
---

The CLI is the daily-driver tool for Reqloom. Three subcommands:

- [`reqloom run`](/cli/run/) — execute an operation chain
- [`reqloom lint`](/cli/lint/) — validate the schema
- [`reqloom import`](/cli/import/) — convert OpenAPI / Postman / Bruno / curl logs

Common flags that work across commands:

- `--project <path>` — path to the project root (defaults to cwd)
- `--env <name>` — select environment file (defaults to `local`)
- `--var key=value` — override env vars at run time
