---
title: Error codes
description: "Every E_* code the engine emits, what triggers it, and whether it's retryable."
---

The engine produces stable error codes for every failure mode. UI / CLI / tests assert on these.

| Code | Class | Retryable | Common cause |
|---|---|---|---|
| `E_SCHEMA_INVALID` | Schema | No | Malformed YAML or schema version mismatch |
| `E_CYCLE` | Schema | No | Circular dependency in chain |
| `E_REF_UNDEFINED` | Schema | No | `{{X.y}}` refers to non-existent producer |
| `E_VAR_UNRESOLVED` | Resolution | No | Required variable couldn't be substituted |
| `E_NETWORK_TIMEOUT` | Network | Yes | Read or connect timeout |
| `E_NETWORK_DNS` | Network | Yes | DNS resolution failed |
| `E_NETWORK_TLS` | Network | No | TLS handshake failure |
| `E_HTTP_5XX` | HTTP | Yes | Server-side 5xx |
| `E_HTTP_4XX` | HTTP | No | Client-side 4xx |
| `E_STATUS_MISMATCH` | HTTP | No | Status code didn't match `expect_status` |
| `E_SESSION_REFRESH_FAILED` | Auth | No | Auth flow returned an error |
| `E_HOOK_FAILURE` | Hook | No | JS pre/post hook threw or timed out |
| `E_EXTRACTION_FAILED` | Extraction | No | JSONPath didn't match the response |
| `E_RESPONSE_PARSE` | Extraction | No | Response declared JSON but didn't parse |
| `E_CANCELLED` | Run | No | User cancelled |

Full taxonomy is in [Engine requirement](/reference/engine-requirement/) §5.
