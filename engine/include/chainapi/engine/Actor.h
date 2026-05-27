// Actor — an identity with its own auth flow

#pragma once

#include <chainapi/engine/Operation.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

/// How an actor authenticates.
///   - `simple`/`chain`:              HTTP-based login flow (one or more steps)
///   - `basic`:                        RFC 7617 Basic auth — no network call
///   - `api_key`:                      static key in header, query, or cookie
///   - `oauth2_client_credentials`:    RFC 6749 §4.4 client credentials grant
///   - `oauth2_password`:              RFC 6749 §4.3 resource owner password grant
///   - `oauth1`:                       RFC 5849 HMAC-SHA1, signed per-request
///   - `aws_sigv4`:                    AWS Signature Version 4, signed per-request
enum class AuthStrategy {
    Simple,
    Chain,
    Basic,
    ApiKey,
    OAuth2ClientCredentials,
    OAuth2Password,
    OAuth1,
    AwsSigV4,
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

    /// Strategy-specific config for non-step-based strategies.
    /// `basic` reads `username`/`password`, `api_key` reads `key`/`location`/`name`, etc.
    /// Values may contain {{X.y}} references resolved at auth time.
    /// Step-based strategies (Simple, Chain) ignore this map.
    std::map<std::string, std::string> authConfig;

    std::chrono::seconds sessionTtl{15 * 60};
    std::optional<SessionRefresh> refresh;

    ActorInjection inject;
};

}  // namespace chainapi::engine
