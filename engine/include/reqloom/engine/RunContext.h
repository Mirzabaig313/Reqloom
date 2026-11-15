// Per-run mutable state: session cache, extraction cache, and recorded steps.

#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/Transport.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

class SecretRegistry;

/// One actor session, lifecycled per run.
struct ActorSession {
    enum class State : std::uint8_t { None, Authenticating, Live, Refreshing };

    /// Per-request signing scheme. Most strategies leave `None`; OAuth 1.0a and AWS SigV4 sign over
    /// the URL / method / params / body, so the executor calls a signer right before each send.
    enum class SigningScheme : std::uint8_t { None, OAuth1HmacSha1, AwsSigV4 };

    State state{State::None};
    std::map<std::string, std::string> variables;  ///< token, user_id, etc.

    /// Request mutations an auth strategy pre-resolves for every operation of the actor; already
    /// resolved, not re-resolved when merged.
    std::map<std::string, std::string> injectHeaders;
    std::map<std::string, std::string> injectQueryParams;

    /// When non-`None`, the executor calls the matching signer (e.g.
    /// `signOAuth1Request`) after inject merging but before `HttpClient::send`.
    SigningScheme signingScheme{SigningScheme::None};

    /// Transport-level auth (mTLS): client cert/key/CA the actor presents on every operation;
    /// non-empty fields overlay the environment transport.
    std::optional<TransportConfig> transport;

    std::chrono::steady_clock::time_point expiresAt;
};

/// One extracted resource instance. Indexed for `{{R[k].x}}` resolution.
struct ResourceInstance {
    std::map<std::string, std::string> variables;

    /// Operation whose response produced this instance; empty when a caller seeded it.
    OperationId producer;

    /// Fields the producing response returned as JSON null.
    std::set<std::string> nullVariables;

    /// Fields the producing response did not contain.
    std::set<std::string> missingVariables;

    /// True when this instance records `name` as null or missing: a lookup stops here rather
    /// than falling back to an older instance's value.
    [[nodiscard]] bool isUnset(const std::string& name) const {
        return nullVariables.contains(name) || missingVariables.contains(name);
    }
};

/// Outcome of one declared assertion evaluated against the final response.
struct AssertionResult {
    std::string name;  ///< Human label, or the expression when unnamed.
    std::string expr;  ///< The predicate that was evaluated.
    bool passed{false};
};

/// One step in a chain.
struct StepResult {
    enum class Status : std::uint8_t {
        Pending,
        Ready,
        Skipped,
        Succeeded,
        Failed,
        Cancelled,
        Blocked
    };

    OperationId op;
    Status status{Status::Pending};
    std::optional<ErrorCode> error;
    int attempts{0};
    std::chrono::milliseconds elapsed{};

    std::string detail;

    /// Set when this row is one poll attempt within a `poll_until` loop
    /// (1-based). Parent operation rows leave this empty.
    std::optional<int> pollAttempt;

    /// Set when this row is one iteration of a `for_each` operation (1-based).
    /// Plain (non-for-each) operation rows leave this empty.
    std::optional<int> forEachIndex;

    /// Results of the operation's declared assertions, in declaration order.
    std::vector<AssertionResult> assertions;
};

struct ExtractionTrace {
    enum class Outcome : std::uint8_t {
        Resolved,        ///< Source path resolved to a non-null value.
        Null,            ///< Source path resolved but the value was null.
        Missing,         ///< Source path did not resolve at all.
        InvalidPattern,  ///< Source kind is regex/xpath and the pattern is malformed.
        Unsupported,     ///< Source kind cannot be resolved by this engine build.
    };

    OperationId op;
    std::string variableName;
    std::string sourcePath;
    Extraction::Source sourceKind{Extraction::Source::JsonPath};
    Outcome outcome{Outcome::Missing};

    /// Resolved value when `outcome == Resolved`. Empty otherwise.
    /// Truncated to ~256 bytes so the timeline doesn't store payloads.
    std::string value;
};

/// One cookie in an actor's jar, with what decides where it is sent (RFC 6265 §5.3).
struct JarCookie {
    std::string name;
    std::string value;
    /// Lowercase host of the response that set it. Empty for a cookie seeded with `setCookie`,
    /// which is sent to any host.
    std::string setterHost;
    std::optional<std::string> domain;  ///< Domain attribute, lowercase, leading dot removed.
    std::optional<std::string> path;    ///< Path attribute; nullopt sends on every path.
    bool secure{false};
    std::optional<std::chrono::system_clock::time_point> expiresAt;
};

/// The mutable state of a single run.

class RunContext {
public:
    RunContext();
    ~RunContext();
    RunContext(const RunContext&) = delete;
    RunContext& operator=(const RunContext&) = delete;
    RunContext(RunContext&&) noexcept;
    RunContext& operator=(RunContext&&) noexcept;

    [[nodiscard]] const ActorSession* session(const ActorId& actor) const noexcept;
    void putSession(const ActorId& actor, ActorSession session);
    void invalidateSession(const ActorId& actor);
    /// Drop every session and cookie jar, including inline-OAuth2 token caches not keyed by an
    /// actor.
    void clearSessions();

    /// Snapshot of the actor's current cookies by name. When two hosts set the same name, the
    /// most recently stored value is shown. Empty when no jar.
    [[nodiscard]] std::map<std::string, std::string> cookies(const ActorId& actor) const;

    /// The actor's cookies with their scope, oldest first.
    [[nodiscard]] std::vector<JarCookie> jarCookies(const ActorId& actor) const;

    /// Insert or replace a cookie with no known setting host (sent to any host). The latest
    /// value wins on name collision (RFC 6265 §5.3 step 11).
    void setCookie(const ActorId& actor, std::string name, std::string value);

    /// Insert `cookie`, replacing one with the same name, domain-or-host and path. Its value is
    /// registered for redaction.
    void storeCookie(const ActorId& actor, JarCookie cookie);

    /// Drop every cookie named `name` from the actor's jar. No-op when absent.
    void removeCookie(const ActorId& actor, const std::string& name);

    /// Drop the cookies named `name` that a response from `host` may delete: ones it set, ones
    /// whose Domain covers it, and ones with no known setting host.
    void removeCookie(const ActorId& actor, const std::string& name, std::string_view host);

    /// Drop the actor's entire jar. Called by `invalidateSession`.
    void clearCookies(const ActorId& actor);

    [[nodiscard]] const std::vector<ResourceInstance>& instances(
        const ResourceId& resource) const noexcept;
    void appendInstance(const ResourceId& resource, ResourceInstance instance);
    /// Drop the instances of `resource` that `producer` recorded earlier, then append
    /// `instances` (each stamped with `producer`) as the newest. Instances from other
    /// producers keep their place.
    void replaceInstances(const ResourceId& resource,
                          const OperationId& producer,
                          std::vector<ResourceInstance> instances);
    void clearExtractions();

    // For-each binding: while set, a bare `{{<resource>.<field>}}` resolves to the instance at this
    // index instead of the most recent. Cleared after the loop; nullopt means no binding.
    void setIteration(const ResourceId& resource, std::optional<std::size_t> index);
    [[nodiscard]] std::optional<std::size_t> iteration(const ResourceId& resource) const noexcept;

    void record(StepResult step);
    [[nodiscard]] const std::vector<StepResult>& steps() const noexcept;

    void recordExtraction(ExtractionTrace trace);
    [[nodiscard]] const std::vector<ExtractionTrace>& extractionTrace() const noexcept;

    /// Secret VALUES known to this context (engine-internal type); each is redacted from every
    /// event, step detail and history row. `putSession` and `setCookie` register credentials
    /// automatically. Thread-safe and usable through a const context.
    [[nodiscard]] SecretRegistry& secretRegistry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Masked form of a captured body (`ResponseReceived::body`): token-like fields masked by name
/// at any depth, then every secret value registered in `ctx` replaced by the marker. Every
/// surface an AI agent or the CLI can read must call this before printing or returning a body.
/// nullopt when the body is not text and so cannot be made safe; omit it then.
[[nodiscard]] std::optional<std::string> maskCapturedBody(std::string_view body,
                                                          const RunContext& ctx);

}  // namespace reqloom::engine
