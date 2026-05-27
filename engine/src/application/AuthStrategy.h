// Authenticator — pluggable actor-authentication strategies.

#pragma once

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace chainapi::engine {

class HttpClient;        // forward — defined in infrastructure
class VariableResolver;  // forward — defined in domain
struct ResolveContext;   // forward — defined in domain (VariableResolver.h)

/// Sink for engine-side observability events emitted from inside auth
/// strategies and the refresh path. The executor binds a closure that
/// dispatches into the engine's emit loop. Empty (default) sink means
/// "drop the event"; tests that don't care about events leave it
/// default-constructed.
using EventSink = std::function<void(const RunEvent&)>;

/// Common dependencies every authenticator needs. Non-owning — must not
/// outlive the engine that created it.
struct AuthDependencies {
    HttpClient* http{nullptr};
    VariableResolver* varResolver{nullptr};
    /// Emit observability events from inside the auth flow. Header
    /// values are masked by the auth strategy before reaching the
    /// sink — same contract as the executor's main path.
    EventSink emit{};
    /// runId / stepIndex to stamp on auth-side RequestPrepared and
    /// ResponseReceived events. The desktop timeline groups by these
    /// fields, so auth events ride on the parent step's index — they
    /// surface as additional rows under the operation that triggered
    /// the auth flow.
    RunId runId{};
    std::size_t stepIndex{0};
};

class Authenticator {
public:
    Authenticator() = default;
    Authenticator(const Authenticator&) = delete;
    Authenticator& operator=(const Authenticator&) = delete;
    Authenticator(Authenticator&&) = delete;
    Authenticator& operator=(Authenticator&&) = delete;
    virtual ~Authenticator() = default;

    /// Run the authenticator's flow. Returns the resulting session
    /// (variables populated) or a `ChainApiError`.
    ///
    /// Authenticators must NOT consult or mutate the `RunContext`'s
    /// session cache — that's the engine's job. They MAY mutate the
    /// per-actor cookie jar via `ctx.setCookie(...)` to absorb
    /// `Set-Cookie` headers that arrive on the auth response.
    [[nodiscard]] virtual std::expected<ActorSession, ChainApiError> authenticate(
        const Actor& actor, RunContext& ctx, const ResolveContext& rctx) = 0;
};

/// Pick a concrete authenticator for `actor`. Returns null only when the
/// actor declares an unsupported `AuthStrategy` enum value — the engine
/// surfaces that as `SessionRefreshFailed`.
[[nodiscard]] std::unique_ptr<Authenticator> selectAuthenticator(const Actor& actor,
                                                                 AuthDependencies deps);

/// Run the actor's `refresh:` block (a single HTTP step with templates,
/// expected status, and extractions) and return the new variable map
/// to merge into the existing session.
///
/// Returns `std::nullopt` (via the unexpected error path) when the actor
/// has no refresh block declared. Returns `SessionRefreshFailed` for any
/// network / status / extraction failure — the caller should treat this
/// as "fall back to full re-auth".
///
/// Templates in the refresh block resolve against the EXISTING session
/// (e.g. `{{user.refresh_token}}`), so the caller must keep the session
/// in the RunContext's cache while calling.
[[nodiscard]] std::expected<std::map<std::string, std::string>, ChainApiError> runRefresh(
    const Actor& actor, RunContext& ctx, const ResolveContext& rctx, AuthDependencies deps);

}  // namespace chainapi::engine
