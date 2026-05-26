// Public façade for libchainapi-engine.
// pImpl + value types only — no Qt UI types, no infra-library types leak.
#pragma once

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>
#include <chainapi/engine/Operation.h>
#include <chainapi/engine/Resource.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

// Forward-declared infrastructure interfaces. Concrete implementations
// live in `engine/src/infrastructure/` and are not part of the public ABI.
class HttpClient;
class SchemaParser;
class HistoryStore;
class SecretStore;
class HookRunner;

/// Result of a run.
struct RunResult {
    RunId runId;
    RunOutcome outcome{RunOutcome::Succeeded};
    std::vector<StepResult> steps;

    [[nodiscard]] bool succeeded() const noexcept { return outcome == RunOutcome::Succeeded; }
};

/// A loaded, validated project. The schema parser produces this; the engine
/// consumes it. Cycles, undefined references, and unsupported versions are
/// caught at parse time and surfaced as `ChainApiError`.
struct Project {
    std::string name;
    std::string defaultEnvironment;
    std::map<ActorId, Actor> actors;
    std::map<ResourceId, Resource> resources;
    std::map<std::string, std::map<std::string, std::string>> environments;
};

/// Per-run options.
struct RunOptions {
    bool dryRun{false};
    bool resetExtractions{false};  ///< Clears the extraction cache before running.
    bool resetSessions{false};     ///< Invalidates all sessions before running.
    std::string environment;       ///< Empty → use project default.
};

class ExecutionEngine {
public:
    /// Dependencies are constructor-injected. Tests substitute fakes;
    /// production wiring lives in `Bootstrapper.cpp` (desktop) or `main.cpp` (CLI).
    struct Dependencies {
        Dependencies();
        Dependencies(std::unique_ptr<HttpClient> http,
                     std::unique_ptr<SchemaParser> schema,
                     std::unique_ptr<HistoryStore> history,
                     std::unique_ptr<SecretStore> secrets,
                     std::unique_ptr<HookRunner> hooks);
        ~Dependencies();

        Dependencies(Dependencies&&) noexcept;
        Dependencies& operator=(Dependencies&&) noexcept;

        Dependencies(const Dependencies&) = delete;
        Dependencies& operator=(const Dependencies&) = delete;

        std::unique_ptr<HttpClient> http;
        std::unique_ptr<SchemaParser> schema;
        std::unique_ptr<HistoryStore> history;
        std::unique_ptr<SecretStore> secrets;
        std::unique_ptr<HookRunner> hooks;
    };

    explicit ExecutionEngine(Dependencies deps);
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;
    ExecutionEngine(ExecutionEngine&&) noexcept;
    ExecutionEngine& operator=(ExecutionEngine&&) noexcept;

    /// Execute a single operation, auto-resolving its dependency chain.
    ///
    /// Returns a populated `RunResult` on success or a `ChainApiError` on
    /// schema-time failures (cycle, undefined reference, etc.). A chain
    /// whose target step fails at runtime returns a `RunResult` with
    /// `outcome == Failed`, not an error — inspect `steps` to find which
    /// step failed.
    std::expected<RunResult, ChainApiError> run(const Project& project,
                                                const OperationId& target,
                                                RunContext& ctx,
                                                const RunOptions& options = {});

    /// Cancel an in-flight run.
    void cancel(RunId run);

    /// Subscribe to streaming run events.
    using EventCallback = std::function<void(const RunEvent&)>;
    void subscribe(EventCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chainapi::engine
