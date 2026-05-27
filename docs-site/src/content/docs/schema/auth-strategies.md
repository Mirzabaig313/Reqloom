---
title: Auth strategies
description: "Six auth strategies: simple, chain, api_key, oauth2_client_credentials, oauth2_password, oauth1."
---

ChainAPI ships six auth strategies covering the patterns 95% of APIs use:

| Strategy | When to use |
|---|---|
| `simple` | Single login request returns a token |
| `chain` | Multi-step (e.g. send-OTP → verify-OTP) |
| `api_key` | Pre-issued static credential |
| `oauth2_client_credentials` | Client-credentials grant |
| `oauth2_password` | Resource-owner password grant |
| `oauth1` | OAuth 1.0a signed requests |

Detailed YAML examples for each are in the [authoring guide](/schema/authoring/).
