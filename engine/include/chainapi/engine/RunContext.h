// Per-run mutable state: session cache, extraction cache, and recorded steps.
// Engine Requirement §3.3, §3.4.
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

/// One actor session, lifecycled per Engine Requirement §4.2.
struct ActorSession {
    enum class State { None, Authenticating, Live, Refreshing };

    State state{State::None};
    std::map<std::string, std::string> variables;          ///< token, user_id, etc.
    std::chrono::steady_clock::time_point expiresAt{};
};

/// One extracted resource instance. Indexed for `{{R[k].x}}` resolution.
struct ResourceInstance {
    std::map<std::string, std::string> variables;
};

/// Status of a single step in a chain. Engine Req §4.1.
struct StepResult {
    enum class Status {
        Pending, Ready, Skipped, Succeeded, Failed, Cancelled, Blocked
    };

    OperationId op;
    Status status{Status::Pending};
    std::optional<ErrorCode> error;
    int attempts{0};
    std::chrono::milliseconds elapsed{};
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

    // Session cache — per actor.
    [[nodiscard]] const ActorSession* session(const ActorId& actor) const noexcept;
    void putSession(const ActorId& actor, ActorSession session);
    void invalidateSession(const ActorId& actor);

    // Extraction cache — list of instances per resource (for `{{R[k].x}}`).
    [[nodiscard]] const std::vector<ResourceInstance>&
        instances(const ResourceId& resource) const noexcept;
    void appendInstance(const ResourceId& resource, ResourceInstance instance);
    void clearExtractions();

    // Step recording.
    void record(StepResult step);
    [[nodiscard]] const std::vector<StepResult>& steps() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chainapi::engine
