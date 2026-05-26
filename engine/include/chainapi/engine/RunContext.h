// Per-run mutable state: session cache, extraction cache, and recorded steps.

#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>
#include <chainapi/engine/Operation.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

/// One actor session, lifecycled per run.
struct ActorSession {
    enum class State { None, Authenticating, Live, Refreshing };

    /// Per-request signing scheme. Most strategies leave this as `None`
    /// and rely on `injectHeaders` for static auth values. OAuth 1.0a
    /// and AWS SigV4 are the exceptions — their signature depends on the
    /// request URL / method / params / body, so the executor calls a
    /// signer right before each send.
    enum class SigningScheme { None, OAuth1HmacSha1, AwsSigV4 };

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

    std::chrono::steady_clock::time_point expiresAt{};
};

/// One extracted resource instance. Indexed for `{{R[k].x}}` resolution.
struct ResourceInstance {
    std::map<std::string, std::string> variables;
};

/// Status of a single step in a chain.
struct StepResult {
    enum class Status { Pending, Ready, Skipped, Succeeded, Failed, Cancelled, Blocked };

    OperationId op;
    Status status{Status::Pending};
    std::optional<ErrorCode> error;
    int attempts{0};
    std::chrono::milliseconds elapsed{};

    /// Human-readable context for this step's outcome — HTTP status,
    /// response body excerpt, missing variable name, etc.
    std::string detail;

    /// Set when this row is one poll attempt within a `poll_until` loop
    /// (1-based). Parent operation rows leave this empty. Renderers can
    /// indent or group these under the parent step row.
    std::optional<int> pollAttempt;
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

    // Extraction cache — list of instances per resource
    [[nodiscard]] const std::vector<ResourceInstance>& instances(
        const ResourceId& resource) const noexcept;
    void appendInstance(const ResourceId& resource, ResourceInstance instance);
    void clearExtractions();

    // Step recording
    void record(StepResult step);
    [[nodiscard]] const std::vector<StepResult>& steps() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chainapi::engine
