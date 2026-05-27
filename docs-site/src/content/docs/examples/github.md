---
title: GitHub REST API example
description: "22-endpoint GitHub validation project: PAT auth, header-based pagination, deep dependency chains."
---

The validation project at `validation/github/` (in the source tree) models a 22-endpoint slice of GitHub's REST API. Highlights:

- Two actors (user + admin PAT) with the same auth strategy but different scopes
- Header-based pagination via `Link: <...>; rel="next"`
- Deep dependency chain: repo → branch → content → pull → merge

This was one of the three Phase 0 validation APIs; the findings document at `validation/github/findings.md` (local-only) details the schema gaps that informed the spec.

Full annotated walkthrough is part of Phase 2 documentation.
