// Authenticator — pluggable actor-authentication strategies.
//

#pragma once

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <memory>

namespace chainapi::engine {

class HttpClient;       // forward — defined in infrastructure
class VariableResolver; // forward — defined in domain
struct ResolveContext;  // forward — defined in domain (VariableResolver.h)

/// Common dependencies every authenticator needs. Owned by the engine;
/// passed by pointer so the contract is unambiguous: the authenticator
/// is non-owning and must not outlive the engine that created it.
/// Using pointers (rather than references) also lets test fakes build
/// the struct in stages and signals "don't store this past the
/// strategy's lifetime" more loudly.
struct AuthDependencies {
    HttpClient*       http{nullptr};
    VariableResolver* varResolver{nullptr};
};

/// Result of a successful authentication: a populated `ActorSession`
/// ready to be cached on the `RunContext`. The state field is set to
/// `Authenticating` — the caller flips it to `Live` and fills in
/// `expiresAt` from `actor.sessionTtl`.
class Authenticator {
public:
    virtual ~Authenticator() = default;

    /// Run the authenticator's flow. Returns the resulting session
    /// (variables populated) or a `ChainApiError`.
    ///
    /// Authenticators must NOT consult or mutate the `RunContext`'s
    /// session cache — that's the engine's job. Read-only access to
    /// the run context is permitted (e.g. cross-actor variable refs
    /// in templates), which is why `ctx` is `const`.
    [[nodiscard]] virtual std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) = 0;
};

/// Pick a concrete authenticator for `actor`. The engine calls this
/// once per actor per run (effectively cached because sessions persist
/// across runs in the same `RunContext`). Returns null only when the
/// actor declares an unsupported `AuthStrategy` enum value — the engine
/// surfaces that as `SessionRefreshFailed`.
[[nodiscard]] std::unique_ptr<Authenticator>
selectAuthenticator(const Actor& actor, AuthDependencies deps);

}  // namespace chainapi::engine
