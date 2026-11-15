// RequestSigners — per-request signing for strategies whose auth header depends on the request
// (URL, method, params). Strategies with static auth values use `ActorSession::injectHeaders`
// instead.
#pragma once

#include <reqloom/engine/RunContext.h>

#include "../infrastructure/http/HttpClient.h"

#include <optional>
#include <string>

namespace reqloom::engine {

/// Test-only seam: when set, OAuth1 signing uses these instead of
/// generating a random nonce / current Unix time.
struct OAuth1TestOverrides {
    std::optional<std::string> nonce;
    std::optional<std::string> timestampSeconds;
};

/// Test-only seam for AWS SigV4: pins the request-time stamp (ISO 8601 basic `YYYYMMDDTHHMMSSZ`);
/// the date scope is derived from it.
struct SigV4TestOverrides {
    std::optional<std::string> amzDate;  ///< e.g. "20150830T123600Z"
    /// When set, receives the canonical request that was signed.
    std::string* canonicalRequestOut{nullptr};
};

/// Sign `req` in-place with OAuth 1.0a HMAC-SHA1 (RFC 5849 §3.4), reading consumer_key /
/// consumer_secret / token / token_secret / realm (optional) from `session.variables` and writing
/// `Authorization: OAuth ...`. Query parameters, and `application/x-www-form-urlencoded` body
/// parameters (§3.4.1.3.1), are folded into the base string. Returns false on failure (missing
/// variable, malformed URL), leaving `req` unmodified.
bool signOAuth1Request(HttpRequest& req,
                       const ActorSession& session,
                       const OAuth1TestOverrides& overrides = {});

/// Sign `req` in-place with AWS Signature Version 4 (`AWS4-HMAC-SHA256`).
/// Reads `access_key`, `secret_key`, `region`, `service` and optional `session_token` (STS) from
/// `session.variables`; canonical request per
/// https://docs.aws.amazon.com/general/latest/gr/sigv4-create-canonical-request.html. Writes
/// `x-amz-date`, `x-amz-content-sha256` (always; required for S3), `x-amz-security-token` (when
/// `session_token` is present) and `Authorization`. The body is SHA-256 hashed into the canonical
/// request; a multipart body is `UNSIGNED-PAYLOAD` for `s3` and refused otherwise. The canonical
/// URI is the path encoded once for `s3`, twice for other services. Atomic on failure: returns
/// false with `req` untouched (missing variable, malformed URL, multipart body for a service other
/// than `s3`, OpenSSL hash failure).
bool signSigV4Request(HttpRequest& req,
                      const ActorSession& session,
                      const SigV4TestOverrides& overrides = {});

/// The reason `signSigV4Request` refuses `req` for its shape rather than for missing
/// credentials or a malformed URL: a multipart body for a service other than `s3`. nullopt
/// when there is none.
[[nodiscard]] std::optional<std::string> sigV4Refusal(const HttpRequest& req,
                                                      const ActorSession& session);

}  // namespace reqloom::engine
