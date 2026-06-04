---
title: Variables & references
description: "Six namespaces of variable references in Reqloom: builtins, actor sessions, resource extractions, environment, secrets."
---

Reqloom's variable substitution syntax is `{{<scope>.<field>}}`. Resolution order:

1. Builtins (`{{$.uuid}}`, `{{$.now}}`, `{{$.faker.*}}`)
2. Actor sessions (`{{<actor>.<var>}}`)
3. Resource extractions (`{{<resource>.<var>}}`)
4. Environment variables (`{{env.<key>}}`)
5. Secrets (`{{secret.<key>}}`)

See the [Variable syntax reference](/reference/variables/) for the full grammar.
