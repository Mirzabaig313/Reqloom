// RequestSigners — per-request signing for strategies whose auth header
// depends on the request itself (URL, method, params).
//
// Strategies that pre-compute static auth values (Basic, API key, OAuth2
// Bearer) do NOT use this path — they populate `ActorSession::injectHeaders`
// instead.
#pragma once

#include <chainapi/engine/RunContext.h>

#include "../infrastructure/http/HttpClient.h"

#include <optional>
#include <string>

namespace chainapi::engine {

/// Test-only seam: when set, OAuth1 signing uses these instead of
/// generating a random nonce / current Unix time.
struct OAuth1TestOverrides {
    std::optional<std::string> nonce;
    std::optional<std::string> timestampSeconds;
};

/// Test-only seam for AWS SigV4 — pin the request-time stamp so test
/// vectors are reproducible. Format must be ISO 8601 basic
/// (`YYYYMMDDTHHMMSSZ`); the date scope (`YYYYMMDD`) is derived from it.
struct SigV4TestOverrides {
    std::optional<std::string> amzDate;  ///< e.g. "20150830T123600Z"
};

/// Sign `req` in-place using OAuth 1.0a HMAC-SHA1 (RFC 5849 §3.4).
/// Reads consumer_key / consumer_secret / token / token_secret / realm
/// (optional) from `session.variables`, builds the base string per the
/// spec, computes the signature, and writes the resulting
/// `Authorization: OAuth ...` header onto `req`.
///
/// `req.url` may carry a query string; query parameters are folded into
/// the signature base. If the body is `application/x-www-form-urlencoded`,
/// those parameters are folded in too (RFC 5849 §3.4.1.3.1).
///
/// Returns false on signing failure (missing required variable, malformed
/// URL) and leaves `req` unmodified.
bool signOAuth1Request(HttpRequest& req,
                       const ActorSession& session,
                       const OAuth1TestOverrides& overrides = {});

/// Sign `req` in-place using AWS Signature Version 4 (the
/// `AWS4-HMAC-SHA256` flavour used by every modern AWS service).
///
/// Reads `access_key`, `secret_key`, `region`, `service` (required) and
/// `session_token` (optional, for STS temporary credentials) from
/// `session.variables`. Builds the canonical request and string-to-sign
/// per https://docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html
/// then writes:
///   - `x-amz-date`
///   - `x-amz-content-sha256` (always set; required for S3, harmless elsewhere)
///   - `x-amz-security-token` (when `session_token` present)
///   - `Authorization: AWS4-HMAC-SHA256 Credential=..., SignedHeaders=..., Signature=...`
///
/// `req.url` may carry a query string. The body (if present) is hashed
/// with SHA-256 and folded into the canonical request; an empty body
/// hashes to the well-known empty-string SHA-256.
///
/// Atomic on failure: when this returns false, `req` is left exactly as
/// the caller passed it. Failure causes are missing required variable,
/// malformed URL, or OpenSSL hash failure.
bool signSigV4Request(HttpRequest& req,
                      const ActorSession& session,
                      const SigV4TestOverrides& overrides = {});

}  // namespace chainapi::engine
