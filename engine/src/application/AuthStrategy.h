// Authenticator — pluggable actor-authentication strategies.

#pragma once

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <memory>

namespace chainapi::engine {

class HttpClient;        // forward — defined in infrastructure
class VariableResolver;  // forward — defined in domain
struct ResolveContext;   // forward — defined in domain (VariableResolver.h)

/// Common dependencies every authenticator needs. Non-owning — must not
/// outlive the engine that created it.
struct AuthDependencies {
    HttpClient* http{nullptr};
    VariableResolver* varResolver{nullptr};
};

class Authenticator {
public:
    virtual ~Authenticator() = default;

    /// Run the authenticator's flow. Returns the resulting session
    /// (variables populated) or a `ChainApiError`.
    ///
    /// Authenticators must NOT consult or mutate the `RunContext`'s
    /// session cache — that's the engine's job.
    [[nodiscard]] virtual std::expected<ActorSession, ChainApiError> authenticate(
        const Actor& actor, const RunContext& ctx, const ResolveContext& rctx) = 0;
};

/// Pick a concrete authenticator for `actor`. Returns null only when the
/// actor declares an unsupported `AuthStrategy` enum value — the engine
/// surfaces that as `SessionRefreshFailed`.
[[nodiscard]] std::unique_ptr<Authenticator> selectAuthenticator(const Actor& actor,
                                                                 AuthDependencies deps);

}  // namespace chainapi::engine
