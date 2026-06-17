// Public façade for libreqloom-engine.
// pImpl + value types only — no Qt UI types, no infra-library types leak.
#pragma once

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/Resource.h>
#include <reqloom/engine/RunContext.h>
#include <reqloom/engine/Transport.h>
#include <reqloom/engine/VariableSuggestion.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::engine {

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
/// caught at parse time and surfaced as `ReqloomError`.
struct Project {
    std::string name;
    std::string defaultEnvironment;
    std::map<ActorId, Actor> actors;
    std::map<ResourceId, Resource> resources;
    std::map<std::string, std::map<std::string, std::string>> environments;
    /// Per-environment transport overrides. Keyed by environment name
    /// (matches the keys of `environments`). Missing entries fall back
    /// to a default-constructed `TransportConfig` (TLS verified, no
    /// proxy, 5s connect timeout) — which is byte-for-byte equivalent
    /// to the engine's behavior before this map existed.
    std::map<std::string, TransportConfig> transport;
};

/// Per-run options.
struct RunOptions {
    bool dryRun{false};
    bool resetExtractions{false};  ///< Clears the extraction cache before running.
    bool resetSessions{false};     ///< Invalidates all sessions before running.
    std::string environment;       ///< Empty → use project default.

    /// Opt-in: include the raw response body on `ResponseReceived` events.
    /// Off by default — bodies stay off the event surface to honor the
    /// redaction-first contract. When on, every response is captured,
    /// including auth/login/refresh responses (which carry tokens) so a
    /// developer can fully inspect and debug each call. Bodies are capped
    /// at `kMaxCapturedBodyBytes` and persisted to the history store, so
    /// past responses can be replayed in full (Postman-style history).
    bool captureResponseBodies{false};
};

/// Upper bound on a captured response body (5 MiB). Larger bodies are
/// truncated to this many bytes on the `ResponseReceived` event.
inline constexpr std::size_t kMaxCapturedBodyBytes = 5U * 1024U * 1024U;

/// One past run, as surfaced to a history view. A public value type
/// mirroring the engine-internal history row, so the desktop/CLI can
/// list prior runs without reaching into infrastructure headers.
/// Timestamps are ISO-8601 UTC strings; `endedAt`/`outcome` are empty
/// until the run terminates (`RunEnded` lands).
struct RunHistoryEntry {
    RunId runId;
    OperationId target;
    std::string envName;
    std::string startedAt;
    std::string endedAt;
    std::string outcome;  ///< Empty, or Succeeded / Failed / Cancelled.
    std::size_t chainSize{0};
    std::int64_t elapsedMs{-1};  ///< Wall-clock run duration in ms; -1 until the run ends.
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
    /// Returns a populated `RunResult` on success or a `ReqloomError` on
    /// schema-time failures (cycle, undefined reference, etc.). A chain
    /// whose target step fails at runtime returns a `RunResult` with
    /// `outcome == Failed`, not an error — inspect `steps` to find which
    /// step failed.
    [[nodiscard]] std::expected<RunResult, ReqloomError> run(const Project& project,
                                                             const OperationId& target,
                                                             RunContext& ctx,
                                                             const RunOptions& options = {});

    /// Resolve a target's execution chain without running it: the
    /// topological order plus the resolved dependency edges (explicit +
    /// implicit `{{var}}`, each tagged with the flowing variable). Lets the
    /// UI draw the real resolved graph. Returns the same schema-time errors
    /// as `run` (cycle, undefined reference).
    [[nodiscard]] std::expected<ResolvedPlan, ReqloomError> resolvePlan(
        const Project& project, const OperationId& target) const;

    /// Enumerate the `{{...}}` references usable at `target`: upstream
    /// extracts, the target actor's session tokens, env vars, declared
    /// secrets, and `$.` built-ins. Pure (resolves the chain, no I/O) so the
    /// editor can call it for `{{` autocomplete. `environment` empty → the
    /// project default. Returns the same schema-time errors as `run`.
    [[nodiscard]] std::expected<std::vector<VariableSuggestion>, ReqloomError> suggestVariables(
        const Project& project,
        const OperationId& target,
        const std::string& environment = {}) const;

    /// Cancel an in-flight run.
    void cancel(RunId run);

    /// Subscribe to streaming run events.
    using EventCallback = std::function<void(const RunEvent&)>;
    void subscribe(EventCallback callback);

    /// Open (creating if missing) the run-history database at `dbPath`.
    /// Once open, every subsequent run's events are persisted, and the
    /// read methods below replay prior runs. Idempotent across repeated
    /// opens of the same path. A no-op `ReqloomError` is returned if the
    /// engine was built without a history store.
    [[nodiscard]] std::expected<void, ReqloomError> openHistory(
        const std::filesystem::path& dbPath);

    /// Past runs, newest-first, capped at `limit` (0 = no limit). Requires
    /// an opened history store (see `openHistory`).
    [[nodiscard]] std::expected<std::vector<RunHistoryEntry>, ReqloomError> listRuns(
        std::size_t limit = 100) const;

    /// Replay every persisted event for one run, in emission order. Lets a
    /// history view reconstruct a past run's timeline. Requires an opened
    /// history store.
    [[nodiscard]] std::expected<std::vector<RunEvent>, ReqloomError> historyEvents(RunId run) const;

    /// Delete all runs from the open history database. Requires an opened
    /// history store.
    std::expected<void, ReqloomError> clearHistory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reqloom::engine
