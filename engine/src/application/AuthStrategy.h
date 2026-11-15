// Authenticator — pluggable actor-authentication strategies.

#pragma once

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/RunContext.h>

#include <chrono>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace reqloom::engine {

class HttpClient;        // forward — defined in infrastructure
class VariableResolver;  // forward — defined in domain
struct ResolveContext;   // forward — defined in domain (VariableResolver.h)

/// Sink for observability events emitted from inside the auth flow; an empty sink drops them.
using EventSink = std::function<void(const RunEvent&)>;

/// Common dependencies every authenticator needs. Non-owning; must not outlive the engine.
struct AuthDependencies {
    HttpClient* http{nullptr};
    VariableResolver* varResolver{nullptr};
    EventSink emit;            ///< Auth-flow events (headers already masked).
    RunId runId{};             ///< Parent step's id, stamped on auth events
    std::size_t stepIndex{0};  ///< so the timeline groups them under that op.

    /// Attach the raw auth response body to `ResponseReceived`. Off by default because auth bodies
    /// carry tokens.
    bool captureResponseBodies{false};

    /// Mirrors `RunOptions::failureExcerptBytes` for login / token endpoint errors.
    std::size_t failureExcerptBytes{kDefaultFailureExcerptBytes};
};

class Authenticator {
public:
    Authenticator() = default;
    Authenticator(const Authenticator&) = delete;
    Authenticator& operator=(const Authenticator&) = delete;
    Authenticator(Authenticator&&) = delete;
    Authenticator& operator=(Authenticator&&) = delete;
    virtual ~Authenticator() = default;

    /// Run the flow and return the resulting session. Must NOT touch the RunContext session cache
    /// (the engine's job); MAY set cookies via `ctx.setCookie(...)`.
    [[nodiscard]] virtual std::expected<ActorSession, ReqloomError> authenticate(
        const Actor& actor, RunContext& ctx, const ResolveContext& rctx) = 0;
};

/// Pick a concrete authenticator for `actor`. Returns null only when the
/// actor declares an unsupported `AuthStrategy` enum value — the engine
/// surfaces that as `SessionRefreshFailed`.
/// Takes `deps` by value so the caller's `std::move` sinks the EventSink instead of copying it.
[[nodiscard]] std::unique_ptr<Authenticator> selectAuthenticator(const Actor& actor,
                                                                 AuthDependencies deps);

/// Subtracted from a token's `expires_in` so a token is never presented just before it expires.
inline constexpr std::chrono::seconds kTokenExpiryMargin{30};

/// How long a session obtained or refreshed for `actor` stays live. `actor.sessionTtl`, except
/// that for an OAuth2 actor whose `variables` carry a positive integer `expires_in` it is
/// `min(expires_in - kTokenExpiryMargin, sessionTtl)` (`expires_in` itself when it is within the
/// margin).
[[nodiscard]] std::chrono::seconds sessionLifetime(
    const Actor& actor, const std::map<std::string, std::string>& variables);

/// Run the actor's `refresh:` block and return the variable map to merge into the session.
/// Returns `SessionRefreshFailed` on a missing refresh block or any network/status/extraction
/// failure; the caller falls back to full re-auth. Templates resolve against the EXISTING
/// session, so the caller must keep it in the RunContext's cache.
[[nodiscard]] std::expected<std::map<std::string, std::string>, ReqloomError> runRefresh(
    const Actor& actor, RunContext& ctx, const ResolveContext& rctx, const AuthDependencies& deps);

}  // namespace reqloom::engine
