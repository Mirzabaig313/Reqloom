// Actor — an identity with its own auth flow

#pragma once

#include <chainapi/engine/Operation.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

/// Auth strategy.
///   - `simple`:  single-shot username/password (or equivalent)
///   - `chain`:   multi-step (e.g. send_otp → verify_otp)
///   - `basic`:   HTTP Basic auth (RFC 7617). No network call — computes
///                `base64(username:password)` and stores it as
///                `session.variables["credential"]`.
///   - `api_key`: API key sent as a header, query param, or cookie
///                (cookie deferred). Stores the resolved key as
///                `session.variables["key"]` and, when `location` +
///                `name` are set, auto-populates `injectHeaders` /
///                `injectQueryParams`.
///   - `oauth2_client_credentials`: RFC 6749 §4.4. POSTs
///                `grant_type=client_credentials` to `token_url`,
///                extracts `access_token`, and auto-injects
///                `Authorization: Bearer <token>`.
///   - `oauth2_password`: RFC 6749 §4.3. Same wire shape as
///                `oauth2_client_credentials` but `grant_type=password`
///                with `username`/`password` in the form body.
///   - `oauth1`:  RFC 5849 OAuth 1.0a, two-legged HMAC-SHA1. Signs
///                per-request; the authenticator sets
///                `signingScheme = OAuth1HmacSha1` and the executor
///                calls `signOAuth1Request` before each send.
///                Three-legged flow is deferred.
///
/// Future strategies (AWS SigV4, etc.) each get their own enum value.
enum class AuthStrategy {
    Simple,
    Chain,
    Basic,
    ApiKey,
    OAuth2ClientCredentials,
    OAuth2Password,
    OAuth1,
};

/// One step in an actor's auth chain.
struct AuthStep {
    std::string id;  ///< Stable id within the actor.
    HttpMethod method{HttpMethod::Post};
    std::string pathTemplate;
    std::map<std::string, std::string> headers;
    std::optional<std::string> bodyTemplate;
    std::optional<int> expectStatus;
    std::vector<Extraction> extractions;
};

/// Optional refresh block.
struct SessionRefresh {
    HttpMethod method{HttpMethod::Post};
    std::string pathTemplate;
    std::map<std::string, std::string> headers;
    std::optional<std::string> bodyTemplate;
    std::vector<Extraction> extractions;
};

/// Headers (and other request artifacts) injected into every operation
/// performed as this actor. Corresponds to the `inject:` block.
struct ActorInjection {
    std::map<std::string, std::string> headers;
};

struct Actor {
    ActorId id;
    std::string description;

    AuthStrategy strategy{AuthStrategy::Simple};
    std::vector<AuthStep> authSteps;  ///< For `simple`, exactly one step.

    /// Strategy-specific configuration. Used by non-step-based strategies —
    /// `basic` reads `username`/`password` here, `api_key` reads
    /// `key`/`location`/`name`, etc. Values may contain {{X.y}} references
    /// resolved at auth time. Step-based strategies (Simple, Chain) ignore
    /// this map.
    std::map<std::string, std::string> authConfig;

    std::chrono::seconds sessionTtl{15 * 60};
    std::optional<SessionRefresh> refresh;

    ActorInjection inject;
};

}  // namespace chainapi::engine
