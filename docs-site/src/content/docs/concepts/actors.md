---
title: Actors
description: "Actor abstraction in ChainAPI: identities with their own auth flows, session caches, and injected headers."
---

Actors are identities with their own auth flows. Each actor defines a sequence of HTTP requests that produce a session, and a set of headers to inject into every operation that runs as this actor.

See [Mental model](/concepts/mental-model/) and [Auth strategies](/schema/auth-strategies/) for the concrete details. Full content for this page is part of Phase 2 documentation.
