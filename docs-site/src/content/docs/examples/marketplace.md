---
title: Marketplace API example
description: "30-endpoint two-sided marketplace sample with admin/vendor/customer actors. The canonical reference project."
---

The bundled `samples/marketplace/` project models a two-sided marketplace:

- 3 actors: admin (email + password), vendor (email + password + refresh), customer (phone + OTP chain)
- 5 resources: products, cart, orders, refunds, reviews
- 27 operations covering the full e-commerce flow

Try it:

```bash
chainapi lint --project samples/marketplace
chainapi run refund.approve --project samples/marketplace
```

The repository structure: see `samples/marketplace/` in the source tree.

Full annotated walkthrough is part of Phase 2 documentation.
