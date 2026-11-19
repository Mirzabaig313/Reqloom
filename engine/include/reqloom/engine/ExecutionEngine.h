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

// Forward-declared infrastructure interfaces; not part of the public ABI.
class HttpClient;
class SchemaParser;
class HistoryStore;
class SecretStore;
class HookRunner;
class ProjectTrustStore;
class HostApprovalStore;
class HostApprover;

/// Result of a run.
struct RunResult {
    RunId runId;
    RunOutcome outcome{RunOutcome::Succeeded};
    std::vector<StepResult> steps;

    /// The masked step rows were stored in run history (`RunOptions::recordResponses` with history
    /// open, under a run id the store reserved), so `recordedSteps(runId)` returns them.
    bool recorded{false};

    [[nodiscard]] bool succeeded() const noexcept { return outcome == RunOutcome::Succeeded; }
};

/// A loaded, validated project, produced by the schema parser and consumed by the engine.
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

    /// Project-wide default auth for operations whose inline auth is `InlineAuthType::Inherit`.
    /// Empty means no auth.
    std::optional<InlineAuth> defaultAuth;

    /// Where `defaultAuth` was defined (the root `auth:` key). Set by the parser, never in YAML.
    SourceLocation defaultAuthSource;

    std::map<ActorId, Actor> actors;
    std::map<ResourceId, Resource> resources;
    std::map<std::string, std::map<std::string, std::string>> environments;
    /// Per-environment transport overrides keyed by environment name; a missing entry means a
    /// default `TransportConfig`.
    std::map<std::string, TransportConfig> transport;

    /// Optional p95 latency budget in milliseconds; 0 means unset. The timeline's p95 marker turns
    /// red on breach.
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

    /// Where each of `loadWarnings` was found, at the same index (file, line, column; line 0 when
    /// the warning is about a whole file). Never written to YAML.
    std::vector<SourceLocation> loadWarningSources;
};

/// Default bytes of response body quoted in a failure detail (4 KiB).
inline constexpr std::size_t kDefaultFailureExcerptBytes = 4096;

/// Largest accepted `RunOptions::failureExcerptBytes` (1 MiB), so one failure cannot store an
/// enormous detail. A larger value is clamped.
inline constexpr std::size_t kMaxFailureExcerptBytes = std::size_t{1024} * 1024;

/// Default bytes of masked response body kept per step for agent-facing output (2 KiB).
inline constexpr std::size_t kDefaultResponseBodyBytes = 2048;

/// Per-run options.
struct RunOptions {
    bool dryRun{false};
    bool resetExtractions{false};  ///< Clears the extraction cache before running.
    bool resetSessions{false};     ///< Invalidates all sessions before running.
    std::string environment;       ///< Empty → use project default.

    /// Opt-in: include the response body (auth responses too) on `ResponseReceived` events, capped
    /// at `kMaxCapturedBodyBytes`. It is unmasked; any surface an AI agent or the CLI can read must
    /// pass it through `maskCapturedBody` first.
    bool captureResponseBodies{false};

    /// Most bytes of the (masked) response body quoted in a failed step's detail and auth endpoint
    /// errors; 0 quotes nothing, values above `kMaxFailureExcerptBytes` are clamped.
    std::size_t failureExcerptBytes{kDefaultFailureExcerptBytes};

    /// The run's results go to an agent-facing surface (the CLI), never to a person's view. Adds
    /// masking the desktop app does not do: values the project uses as credentials (by where it
    /// sends them, by credential-like names, cookies a response sets) are masked from the moment
    /// they appear, failure excerpts mask credential-named fields, and every StepResult is
    /// masked again when the run ends. Off (the default), events, history and results are masked
    /// exactly as before.
    bool agentOutput{false};

    /// Read `{{secret.NAME}}` from `REQLOOM_SECRET_<NAME>` before the keychain (FR-11.5). Only the
    /// CLI sets this: a desktop app started from a terminal must not let the environment override
    /// Manage Secrets.
    bool secretsFromEnvironment{false};

    /// Agent-facing output: keep each step's response body and extracted values on its
    /// `StepResult` (`response`, `extracted`). Before `run()` returns they are masked with every
    /// secret the run learned, credentials sent later in the run included, and only then cut.
    bool recordResponses{false};

    /// With `recordResponses`, the most bytes of masked body kept per step row; 0 keeps none.
    /// Values above `kMaxFailureExcerptBytes` are clamped.
    std::size_t responseBodyBytes{kDefaultResponseBodyBytes};

    /// Asks the person to approve a host before a credential is sent to it (FR-15.9); not owned,
    /// must outlive the run. Null means nobody can be asked: a request carrying a credential to a
    /// host that is not approved fails before it is sent.
    HostApprover* hostApprover{nullptr};

    /// Host keys (`hostKeyForEntry`) approved for this run only and never stored: the CLI fills it
    /// from `REQLOOM_APPROVED_HOSTS` for CI. Still limited by an actor's `allowed_hosts`.
    std::vector<std::string> approvedHostsForRun;

    /// Who started the run, recorded on `RunStarted` and in run history (FR-15.8). For display
    /// only.
    RunInitiator initiator{};
};

/// Upper bound on a captured response body (5 MiB); larger bodies are truncated.
inline constexpr std::size_t kMaxCapturedBodyBytes = std::size_t{5} * 1024 * 1024;

/// One past run for a history view. Timestamps are ISO-8601 UTC; `endedAt`/`outcome` are empty
/// until `RunEnded` lands.
struct RunHistoryEntry {
    RunId runId;
    OperationId target;
    std::string envName;
    std::string startedAt;
    std::string endedAt;
    std::string outcome;  ///< Empty, or Succeeded / Failed / Cancelled.
    std::size_t chainSize{0};
    std::int64_t elapsedMs{-1};  ///< Wall-clock run duration in ms; -1 until the run ends.
    RunInitiator initiator{};    ///< Who started it; `App` for runs recorded before FR-15.8.
    /// The run's masked step rows were recorded (`recordedSteps` can read them).
    bool hasRecordedSteps{false};
};

class ExecutionEngine {
public:
    /// Dependencies are constructor-injected; wiring lives in `Bootstrapper.cpp` (desktop) or
    /// `main.cpp` (CLI).
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
        /// Which folder each project id belongs to, checked before any secret; with `secrets` but
        /// no `trust`, a run that references secrets fails.
        std::unique_ptr<ProjectTrustStore> trust;
        /// Hosts this user approved per project (FR-15.9). Without it no host is approved: a
        /// request carrying a credential, secret or session cookie fails before it is sent, unless
        /// `RunOptions` approves its host for the run or the run's approver says yes.
        std::unique_ptr<HostApprovalStore> hostApprovals;
    };

    explicit ExecutionEngine(Dependencies deps);
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;
    ExecutionEngine(ExecutionEngine&&) noexcept;
    ExecutionEngine& operator=(ExecutionEngine&&) noexcept;

    /// Execute a single operation, auto-resolving its dependency chain.
    ///
    /// Returns `ReqloomError` for schema-time failures (cycle, undefined reference). A chain whose
    /// target fails at runtime returns a `RunResult` with `outcome == Failed`; inspect `steps`.
    [[nodiscard]] std::expected<RunResult, ReqloomError> run(const Project& project,
                                                             const OperationId& target,
                                                             RunContext& ctx,
                                                             const RunOptions& options = {});

    /// Resolve a target's execution chain without running it: topological order plus resolved
    /// edges, each tagged with the flowing variable. Same schema-time errors as `run`.
    [[nodiscard]] std::expected<ResolvedPlan, ReqloomError> resolvePlan(
        const Project& project, const OperationId& target) const;

    /// Enumerate the `{{...}}` references usable at `target` (upstream extracts, actor session
    /// tokens, env vars, secrets, `$.` built-ins) for `{{` autocomplete. No I/O; `environment`
    /// empty means the project default. Same schema-time errors as `run`.
    [[nodiscard]] std::expected<std::vector<VariableSuggestion>, ReqloomError> suggestVariables(
        const Project& project,
        const OperationId& target,
        const std::string& environment = {}) const;

    /// Cancel an in-flight run.
    void cancel(RunId run);

    /// Subscribe to streaming run events.
    using EventCallback = std::function<void(const RunEvent&)>;
    void subscribe(EventCallback callback);

    /// Open (creating if missing) the run-history database at `dbPath`; afterwards every run is
    /// persisted. Idempotent for the same path. Errors if the engine was built without a history
    /// store.
    [[nodiscard]] std::expected<void, ReqloomError> openHistory(
        const std::filesystem::path& dbPath);

    /// Open an existing run-history database for reading only (`listRuns`, `historyEvents`,
    /// `recordedSteps`); nothing is created or upgraded, and runs are not recorded. Errors when
    /// the file is missing or older than this build's schema.
    [[nodiscard]] std::expected<void, ReqloomError> openHistoryReadOnly(
        const std::filesystem::path& dbPath);

    /// Past runs, newest-first, capped at `limit` (0 = no limit). Requires `openHistory`.
    [[nodiscard]] std::expected<std::vector<RunHistoryEntry>, ReqloomError> listRuns(
        std::size_t limit = 100) const;

    /// Replay every persisted event for one run in emission order. Requires `openHistory`.
    [[nodiscard]] std::expected<std::vector<RunEvent>, ReqloomError> historyEvents(RunId run) const;

    /// The step rows of a run that recorded them (`RunOptions::recordResponses` with history
    /// open), in `RunResult::steps` order, already masked for agent-facing output; empty for a run
    /// that recorded none (every desktop run). Requires `openHistory`. Agent-facing surfaces read
    /// past runs only through this, never through `historyEvents`, which keep what a person saw.
    [[nodiscard]] std::expected<std::vector<StepResult>, ReqloomError> recordedSteps(
        RunId run) const;

    /// Delete all runs from the open history database. Requires `openHistory`.
    std::expected<void, ReqloomError> clearHistory();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reqloom::engine
