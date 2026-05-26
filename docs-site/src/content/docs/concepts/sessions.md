---
title: Sessions & caching
description: "Per-actor session caching, TTL, automatic refresh, and the rules around when sessions are reused vs re-authenticated."
---

Each actor's auth flow produces a session: a token plus extracted variables, with a configurable TTL. Sessions are cached across operations within a run; the engine automatically reuses them if live, refreshes them if expired (and `session.refresh` is configured), or re-authenticates on `401`.

See [Engine requirement](/reference/engine-requirement/) §3.3 for the full session lifecycle.
