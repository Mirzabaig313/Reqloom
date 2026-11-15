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
class ProjectTrustStore;

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

    /// Folder holding reqloom.yaml, as an absolute path. Set by `parseProject`, never in the
    /// YAML; empty for an in-memory project.
    std::filesystem::path rootDir;

    /// Stable project identifier (`id:` in reqloom.yaml); namespaces the project's keychain
    /// secrets. Empty means unset: a run that needs secrets then fails.
    std::string id;

    /// Process environment variables `{{$.env.NAME}}` may read (`allowed_env:`); other names
    /// stay unresolved so a project file cannot exfiltrate them.
    std::vector<std::string> allowedEnv;

    /// Project-wide default auth. Operations whose inline auth is
    /// `InlineAuthType::Inherit` resolve to this (the "inherit from parent"
    /// behavior). Empty means those operations run with no auth.
    std::optional<InlineAuth> defaultAuth;

    std::map<ActorId, Actor> actors;
    std::map<ResourceId, Resource> resources;
    std::map<std::string, std::map<std::string, std::string>> environments;
    /// Per-environment transport overrides. Keyed by environment name
    /// (matches the keys of `environments`). Missing entries fall back
    /// to a default-constructed `TransportConfig` (TLS verified, no
    /// proxy, 5s connect timeout) — which is byte-for-byte equivalent
    /// to the engine's behavior before this map existed.
    std::map<std::string, TransportConfig> transport;

    /// Optional latency budget for the run's p95 response time, in
    /// milliseconds. 0 means unset (no SLO). Surfaced on the timeline's
    /// latency chart: the p95 marker turns red when the run breaches it.
    int latencySloP95Ms{0};

    /// Root `failure_message.show`: JSONPaths of the response parts a status-mismatch failure
    /// quotes instead of the default excerpt. Empty means the default excerpt.
    std::vector<std::string> failureMessageShow;

    /// Where each environment was defined, keyed like `environments`. Set by the parser,
    /// never written to YAML.
    std::map<std::string, SourceLocation> environmentSources;

    /// The schema files the parser read, keyed by canonical path, valued by a hash of their
    /// bytes at load. Never written to YAML. Pass `&loadedFiles` to `writeProject` so a save
    /// prunes only these files and refuses files that changed or appeared since load.
    std::map<std::filesystem::path, std::size_t> loadedFiles;

    /// Non-fatal findings from the load (unknown keys, imports that matched nothing), each
    /// prefixed with `file:line:column:`. Never written to YAML.
    std::vector<std::string> loadWarnings;
};

/// Default bytes of response body quoted in a failure detail (4 KiB).
inline constexpr std::size_t kDefaultFailureExcerptBytes = 4096;

/// Largest accepted `RunOptions::failureExcerptBytes` (1 MiB), so one failure cannot store an
/// enormous detail. A larger value is clamped.
inline constexpr std::size_t kMaxFailureExcerptBytes = std::size_t{1024} * 1024;

/// Per-run options.
struct RunOptions {
    bool dryRun{false};
    bool resetExtractions{false};  ///< Clears the extraction cache before running.
    bool resetSessions{false};     ///< Invalidates all sessions before running.
    std::string environment;       ///< Empty → use project default.

    /// Opt-in: include the response body on `ResponseReceived` events, including auth
    /// responses. The body is the person's own view: it reaches subscribers and the history
    /// store exactly as received (tokens included), capped at `kMaxCapturedBodyBytes`. A
    /// surface an AI agent or the CLI can read must pass it through `maskCapturedBody` first.
    bool captureResponseBodies{false};

    /// Most bytes of the (masked) response body quoted in a failed step's detail when the
    /// status does not match, and in auth login / token endpoint errors. 0 quotes nothing;
    /// values above `kMaxFailureExcerptBytes` are clamped.
    std::size_t failureExcerptBytes{kDefaultFailureExcerptBytes};
};

/// Upper bound on a captured response body (5 MiB). Larger bodies are
/// truncated to this many bytes on the `ResponseReceived` event.
inline constexpr std::size_t kMaxCapturedBodyBytes = std::size_t{5} * 1024 * 1024;

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
        /// Which folder each project id belongs to, checked before any secret. With `secrets` set
        /// and no `trust`, a run that references secrets fails. `makeDefaultDependencies()` sets
        /// it.
        std::unique_ptr<ProjectTrustStore> trust;
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
