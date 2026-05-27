---
title: Common pitfalls
description: "The 10 most common schema bugs and their fixes, distilled from real-world LLM-generated and hand-written schemas."
---

Every bug in this list happened on a real schema during validation. Patterns to avoid:

1. **Wrong `expect_status`** — `200` vs `201` vs `204` mismatches
2. **Cross-actor session-var reference** — `{{employer.org_id}}` when employer isn't authenticated
3. **Missing OTP / CSRF prerequisite** — auth chains that need a setup step
4. **Hallucinated body fields** — fields the backend rejects
5. **Hardcoded unique value** — works once, fails on re-run
6. **Wrong JSONPath for extraction** — schema vs response shape mismatch
7. **Mixed `body:` and `body_form:`** — only one per operation
8. **Trailing slash mismatch** — backends are strict
9. **`baseUrl` includes the version** — leads to doubled paths
10. **Unquoted YAML scalars** — `otp: 123456` parses as int

The full list with concrete fixes is in the [authoring guide](/schema/authoring/) §Common pitfalls.
