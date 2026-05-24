// Public façade for libchainapi-engine. Project Layout §3.
//
// pImpl + value types only — no Qt UI types appear here, no infra-library
// types leak. Phase B (post-MVP) extracting the engine to a separate
// process or rewriting the implementation in Rust is a build-system change
// rather than a rewrite because of this surface.
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

/// A loaded, validated project. The schema parser produces this; the
/// engine consumes it. Cycles, undefined references, and unsupported
/// versions are caught at parse time and surfaced as `ChainApiError`.
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
    bool resetExtractions{false};  ///< "Reset Cache" — Engine Req AC-3.4.2.
    bool resetSessions{false};     ///< "Send Cleanly" — AC-3.4.3.
    std::string environment;       ///< Empty → use project default.
};

class ExecutionEngine {
public:
    /// Dependencies are constructor-injected. Tests substitute fakes;
    /// production wiring lives in `Bootstrapper.cpp` (desktop) or
    /// `main.cpp` (cli).
    struct Dependencies {
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
    /// Returns a populated `RunResult` on success or a `ChainApiError`
    /// on schema-time failures (cycle, undefined reference, etc.). A
    /// chain whose target step fails at runtime returns a `RunResult`
    /// with `outcome == Failed`, not an error — the caller inspects
    /// `steps` to discover which step failed.
    std::expected<RunResult, ChainApiError> run(const Project& project,
                                                const OperationId& target,
                                                RunContext& ctx,
                                                const RunOptions& options = {});

    /// Cancel an in-flight run. Engine Req §3.8.
    void cancel(RunId run);

    /// Subscribe to streaming run events. Engine Req §10.
    using EventCallback = std::function<void(const RunEvent&)>;
    void subscribe(EventCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chainapi::engine
