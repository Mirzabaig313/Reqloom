---
title: Variable syntax reference
description: "Every namespace, every builtin, the full grammar for {{X.y}} references."
---

Variable references use the syntax `{{<scope>.<field>}}`. Six scopes:

| Scope | Source | Example |
|---|---|---|
| `$` | Builtins | `{{$.uuid}}`, `{{$.now}}`, `{{$.faker.email}}`, `{{$.env.HOME}}` |
| `<actor>` | Actor session vars | `{{vendor.token}}` |
| `<resource>` | Resource extractions (most-recent) | `{{order.order_id}}` |
| `<resource>[N]` | Resource extractions (indexed) | `{{order[2].order_id}}` |
| `env` | Environment variables | `{{env.baseUrl}}` |
| `secret` | OS keychain | `{{secret.STRIPE_KEY}}` |

Resolution order: builtins → actor → resource → env → secret.

Full content for this page is part of Phase 2 documentation.
