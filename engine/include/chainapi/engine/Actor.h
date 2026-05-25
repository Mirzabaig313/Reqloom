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
///   - `basic`:   HTTP Basic auth (RFC 7617). No network call — the
///                authenticator computes `base64(username:password)`
///                and stores it as `session.variables["credential"]`.
///                The actor's `inject` block typically references it as
///                `Authorization: "Basic {{<actor>.credential}}"`.
///   - `api_key`: API key sent as a header, query parameter, or cookie
///                (cookie deferred ). No network
///                call. The authenticator stores the resolved key as
///                `session.variables["key"]` AND, when `location` +
///                `name` are configured, auto-populates the session's
///                `injectHeaders` / `injectQueryParams`. Hybrid model:
///                a one-liner config gets auto-injection; a manual
///                `inject:` block also works against the variable.
///   - `oauth2_client_credentials`: RFC 6749 §4.4. POSTs
///                `grant_type=client_credentials` to `token_url`,
///                extracts `access_token` from the JSON response,
///                stores it as `session.variables["access_token"]`,
///                and auto-injects `Authorization: Bearer <token>`
///                into every operation owned by the actor.
///   - `oauth2_password`: RFC 6749 §4.3 (resource-owner password
///                credentials). Same wire shape as
///                `oauth2_client_credentials` but `grant_type=password`
///                and the form body carries `username`/`password` in
///                addition to the client credentials. Same
///                Bearer auto-injection.
///
/// Future named strategies (OAuth1, AWS SigV4) each
/// get their own enum value
enum class AuthStrategy {
    Simple,
    Chain,
    Basic,
    ApiKey,
    OAuth2ClientCredentials,
    OAuth2Password,
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
/// performed as this actor.  `inject:` block.
struct ActorInjection {
    std::map<std::string, std::string> headers;
};

struct Actor {
    ActorId id;
    std::string description;

    AuthStrategy strategy{AuthStrategy::Simple};
    std::vector<AuthStep> authSteps;  ///< For `simple`, exactly one step.

    /// Strategy-specific configuration . Used by
    /// non-step-based strategies — `basic` reads `username`/`password`
    /// here, future api_key reads `key`/`location`/`name`, etc.
    /// Values may contain {{X.y}} references resolved at auth time.
    /// Step-based strategies (Simple, Chain) ignore this map.
    std::map<std::string, std::string> authConfig;

    std::chrono::seconds sessionTtl{15 * 60};
    std::optional<SessionRefresh> refresh;

    ActorInjection inject;
};

}  // namespace chainapi::engine
