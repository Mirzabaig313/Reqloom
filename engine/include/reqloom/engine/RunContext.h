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
#include <string>
#include <vector>

namespace reqloom::engine {

/// One actor session, lifecycled per run.
struct ActorSession {
    enum class State : std::uint8_t { None, Authenticating, Live, Refreshing };

    /// Per-request signing scheme. Most strategies leave this as `None`
    /// and rely on `injectHeaders` for static auth values. OAuth 1.0a
    /// and AWS SigV4 are the exceptions — their signature depends on the
    /// request URL / method / params / body, so the executor calls a
    /// signer right before each send.
    enum class SigningScheme : std::uint8_t { None, OAuth1HmacSha1, AwsSigV4 };

    State state{State::None};
    std::map<std::string, std::string> variables;  ///< token, user_id, etc.

    /// Auth strategies may pre-resolve request mutations here so the engine
    /// adds them to every operation owned by the actor without requiring an
    /// explicit `inject:` block. Values are already resolved — the engine
    /// does NOT re-resolve them when merging into a request.
    std::map<std::string, std::string> injectHeaders;
    std::map<std::string, std::string> injectQueryParams;

    /// When non-`None`, the executor calls the matching signer (e.g.
    /// `signOAuth1Request`) after inject merging but before `HttpClient::send`.
    SigningScheme signingScheme{SigningScheme::None};

    /// Transport-level auth (mTLS): client cert/key/CA the actor presents on
    /// every operation it owns. Set by the mTLS authenticator; the executor
    /// overlays the non-empty fields onto each request's transport (composing
    /// with, not replacing, the environment transport).
    std::optional<TransportConfig> transport;

    std::chrono::steady_clock::time_point expiresAt;
};

/// One extracted resource instance. Indexed for `{{R[k].x}}` resolution.
struct ResourceInstance {
    std::map<std::string, std::string> variables;
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
    /// Empty when the operation declares none.
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

/// The mutable state of a single run.

class RunContext {
public:
    RunContext();
    ~RunContext();
    RunContext(const RunContext&) = delete;
    RunContext& operator=(const RunContext&) = delete;
    RunContext(RunContext&&) noexcept;
    RunContext& operator=(RunContext&&) noexcept;

    // Session cache — per actor
    [[nodiscard]] const ActorSession* session(const ActorId& actor) const noexcept;
    void putSession(const ActorId& actor, ActorSession session);
    void invalidateSession(const ActorId& actor);

    /// Snapshot of the actor's current cookies. Empty when no jar.
    [[nodiscard]] std::map<std::string, std::string> cookies(const ActorId& actor) const;

    /// Insert or replace a cookie. Mirrors RFC 6265 §5.3 step 11:
    /// the latest Set-Cookie wins on name collision.
    void setCookie(const ActorId& actor, std::string name, std::string value);

    /// Drop the actor's entire jar. Called by `invalidateSession`.
    void clearCookies(const ActorId& actor);

    // Extraction cache — list of instances per resource
    [[nodiscard]] const std::vector<ResourceInstance>& instances(
        const ResourceId& resource) const noexcept;
    void appendInstance(const ResourceId& resource, ResourceInstance instance);
    void clearExtractions();

    // For-each iteration binding. While set, a bare `{{<resource>.<field>}}`
    // resolves to that resource's instance at the bound index (instead of the
    // most-recent), so a for-each body iterates the list. Cleared after the
    // loop. nullopt means "no binding" (normal most-recent resolution).
    void setIteration(const ResourceId& resource, std::optional<std::size_t> index);
    [[nodiscard]] std::optional<std::size_t> iteration(const ResourceId& resource) const noexcept;

    // Step recording
    void record(StepResult step);
    [[nodiscard]] const std::vector<StepResult>& steps() const noexcept;

    void recordExtraction(ExtractionTrace trace);
    [[nodiscard]] const std::vector<ExtractionTrace>& extractionTrace() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reqloom::engine
