---
title: Cheat sheet
description: "Quick-reference table of common schema patterns and CLI flags."
---

| You want to... | Pattern |
|---|---|
| Pass a body field unique per run | `{{env.X}}` + `--var X=value` |
| Pass a body field unique within the run | `{{$.uuid}}` |
| Reference an ID from a previous response | `{{<resource>.<extracted_name>}}` |
| Reference an actor's auth token | `{{<actor>.<token_var>}}` (in `inject.headers`) |
| Force a step to re-run | `force: true` on the op |
| Make an op public | Omit the `actor:` field entirely |
| Send form-encoded body | Use `body_form:` instead of `body:` |
| Run against a different env | `--env staging` |
| Preview the chain without sending | `--dry-run` |
| Override an env var per run | `--var key=value` |
