---
title: Stripe API example
description: "24-endpoint Stripe validation project: form-encoded bodies, idempotency keys, multi-tenant headers (Stripe-Account)."
---

The validation project at `validation/stripe/` models a 24-endpoint slice of Stripe's API. Highlights:

- Form-encoded bodies (`application/x-www-form-urlencoded`)
- Mandatory `Idempotency-Key` headers via `{{$.uuid}}`
- Multi-tenant via `Stripe-Account` header — same credential, different acting identity per request (modeled as two actors with different `inject.headers`)
- 5 resources spanning customer → payment_method → payment_intent → refund

Full annotated walkthrough is part of Phase 2 documentation.
