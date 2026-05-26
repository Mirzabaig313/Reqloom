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

}  // namespace chainapi::engine
