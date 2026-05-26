---
title: Contributing
description: "How to contribute: development workflow, code style, the architectural firewall, the boundary check, PR guidelines."
---

Contributing guidelines:

- Apache 2.0 license; sign-off required (DCO)
- Conventional commits (`feat:`, `fix:`, `docs:`, etc.)
- Run `./tools/pre-push-check.sh` before pushing
- Engine changes must keep the architectural firewall intact (no Qt UI deps)
- Tests required for all new features and bug fixes (80%+ coverage in domain layer)

The full contribution guide is in [`AGENTS.md`](https://github.com/chainapi/chainapi/blob/main/AGENTS.md) in the source tree.

Polished contributor docs are part of Phase 2 documentation.
