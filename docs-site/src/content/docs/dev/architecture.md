---
title: Architecture
description: "The two-phase architecture: in-process engine for MVP, extractable to a separate process or Rust later."
---

Reqloom's architecture is documented in [`doc/Reqloom - Project Layout.md`](https://github.com/Mirzabaig313/Reqloom/blob/main/doc/Reqloom%20-%20Project%20Layout.md).

Key principles:

- **Engine boundary** — `libreqloom-engine` has no Qt UI dependency. Mechanically enforced via CMake link guards, CI grep checks, and pImpl public headers.
- **Layered C++** — domain → application → infrastructure, dependencies pointing inward only.
- **Phase B option** — the engine is constructable as an in-process library today and extractable to a separate process or rewritten in Rust later. Architectural guardrails make this a build-system change, not a rewrite.

