---
title: What is ChainAPI?
description: "A workflow-aware API testing tool that auto-resolves request dependency chains. Built for backend developers and QAs who work with multi-actor SaaS APIs."
---

## The problem

Modern SaaS backends are multi-role and deeply interconnected. To test a
single endpoint, developers and QAs often need to:

1. Log in as multiple actors (admin, vendor, customer, manager)
2. Chain 3–6 API calls to obtain prerequisite IDs
3. Manually copy tokens and IDs between Postman/Bruno/Insomnia tabs
4. Repeat this every time tokens expire or test data is reset

A representative example — testing **"admin approves a customer refund"**
in a marketplace API requires this chain:

```
Admin login            → admin_token
Vendor login           → vendor_token + vendor_id
Customer login         → customer_token + customer_id
Create product         → product_id            (vendor)
Place order            → order_id              (customer)
Pay for order          → payment_id            (customer)
Request refund         → refund_id             (customer)
Approve refund         ← target endpoint       (admin)
```

That's **8 manual requests** with **7 IDs** to copy-paste, just to test
one endpoint. Across a feature with 20 endpoints, this is hours of
mechanical work per test cycle.

## Why existing tools fall short

| Tool | Limitation |
|------|------------|
| **Postman** | Manual scripting; collections grow unwieldy; no actor abstraction |
| **Postman Flows** | Visual flows require wiring every relationship by hand |
| **Bruno** | Same scripting model as Postman; git-friendly but no dependency awareness |
| **Insomnia** | Manual chaining; no role/actor abstraction |
| **Hurl / Stepci** | Linear scripts; require authoring entire workflows for every endpoint |
| **Apidog** | Better than Postman but still requires manual workflow definition |

None of them model the API as a dependency graph. They treat it as a
flat list of requests.

## What ChainAPI does

ChainAPI treats your API as a graph of **resources**, **actors**, and
**dependencies**. Define each actor (auth flow) and each resource
(endpoints + dependencies) **once**. Then click any endpoint and the
engine auto-resolves the entire chain — login, prerequisites, target
call — and executes them in the correct order.

The same schema also powers an **AI importer** that reads your existing
API documentation (OpenAPI, Markdown, curl logs) and bootstraps the
project in minutes.

## Who it's for

- **Backend engineers** testing their own endpoints during development
- **QA engineers** running regression flows across admin, web, and mobile
- **Solo developers** building multi-role SaaS who hit the same pain on
  every project

## What it isn't

- A replacement for Postman in single-request, ad-hoc exploration
- A load testing tool (use k6 / Gatling)
- A real-time collaboration tool (yet — git is the sharing model today)

## How it compares

ChainAPI's wedge is **multi-actor SaaS APIs** — the dominant shape of
modern backend work. Tools that don't model actors as a first-class
concept end up with a forest of scripts that nobody owns. ChainAPI's
schema makes the actor abstraction load-bearing, so the dependency
graph becomes maintainable.

## Next steps

- [Install ChainAPI](/start/install/)
- [Take the 5-minute tour](/start/tour/)
- [Read the schema authoring guide](/schema/authoring/)
