// Observability events emitted by the engine. Consumed by the desktop
// timeline UI and the CLI renderers.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Operation.h>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace reqloom::engine {

struct RunId {
    std::uint64_t value{0};
    auto operator<=>(const RunId&) const = default;
};

using TimePoint = std::chrono::system_clock::time_point;

enum class SkipReason : std::uint8_t { SessionValid, ExtractionCached };

/// Replaces every redacted value in events, details and history; stable so renderers can match on
/// it.
inline constexpr std::string_view kRedactedHeaderValue = "<redacted>";

/// Who started a run (PRD FR-15.8): a person in the app, a person at the command line, or a named
/// agent. Self-reported by the caller and shown for information only; never used to decide what a
/// run may do.
struct RunInitiator {
    enum class Kind : std::uint8_t { App, Cli, Agent };
    Kind kind{Kind::App};
    std::string name;  ///< The agent's name for `Agent`; empty otherwise.
};

/// Stable name of `kind` in run history and machine output: "app", "cli", "agent".
[[nodiscard]] constexpr std::string_view runInitiatorKindName(RunInitiator::Kind kind) noexcept {
    switch (kind) {
        case RunInitiator::Kind::App:
            return "app";
        case RunInitiator::Kind::Cli:
            return "cli";
        case RunInitiator::Kind::Agent:
            return "agent";
    }
    return "app";
}

/// Inverse of `runInitiatorKindName`; `App` for anything else (rows written before FR-15.8).
[[nodiscard]] constexpr RunInitiator::Kind runInitiatorKindFromName(
    std::string_view name) noexcept {
    if (name == "cli") {
        return RunInitiator::Kind::Cli;
    }
    if (name == "agent") {
        return RunInitiator::Kind::Agent;
    }
    return RunInitiator::Kind::App;
}

struct RunStarted {
    RunId runId;
    OperationId target;
    std::size_t chainSize{};
    std::string envName;
    TimePoint at;
    RunInitiator initiator{};
};

struct StepStarted {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    int attempt{1};
    TimePoint at;
};

struct StepSkipped {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    SkipReason reason{};
    TimePoint at;
};

/// What a request / response pair was for. Auth exchanges carry the step index of the operation
/// that needed the session, so only `Operation` and `PollAttempt` are that operation's own.
enum class ExchangeKind : std::uint8_t {
    Operation,        ///< The operation's own request (each retry included).
    SignIn,           ///< One request of an actor's sign-in flow (login steps, token endpoint).
    SessionRefresh,   ///< An actor's `session.refresh:` request.
    InlineAuthToken,  ///< The token request of an operation's inline OAuth2 auth.
    PollAttempt,      ///< One `poll_until` status request.
};

/// Stable name of `kind`, used in run history and by the UIs: "operation", "sign_in",
/// "session_refresh", "inline_auth_token", "poll_attempt".
[[nodiscard]] constexpr std::string_view exchangeKindName(ExchangeKind kind) noexcept {
    switch (kind) {
        case ExchangeKind::Operation:
            return "operation";
        case ExchangeKind::SignIn:
            return "sign_in";
        case ExchangeKind::SessionRefresh:
            return "session_refresh";
        case ExchangeKind::InlineAuthToken:
            return "inline_auth_token";
        case ExchangeKind::PollAttempt:
            return "poll_attempt";
    }
    return "operation";
}

/// Inverse of `exchangeKindName`; nullopt for a name this build does not know.
[[nodiscard]] constexpr std::optional<ExchangeKind> exchangeKindFromName(
    std::string_view name) noexcept {
    for (const auto kind : {ExchangeKind::Operation,
                            ExchangeKind::SignIn,
                            ExchangeKind::SessionRefresh,
                            ExchangeKind::InlineAuthToken,
                            ExchangeKind::PollAttempt}) {
        if (exchangeKindName(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

struct RequestPrepared {
    RunId runId;
    std::size_t stepIndex{};
    HttpMethod method{};
    std::string url;  ///< Sanitized display URL; authority/components opaque, not replayable.
    std::vector<std::pair<std::string, std::string>> maskedHeaders;
    std::size_t bodySize{};
    TimePoint at;
    ExchangeKind exchange{ExchangeKind::Operation};
};

struct ResponseReceived {
    RunId runId;
    std::size_t stepIndex{};
    int status{};
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t bodySize{};
    std::chrono::milliseconds elapsed{};
    TimePoint at;

    /// Response body as received, populated only when the run opted in via
    /// `RunOptions::captureResponseBodies`. Not masked: it is shown to the person and stored in
    /// run history as it was, tokens included. Agent-facing surfaces (CLI, AI tools) must
    /// print only `maskCapturedBody(*body, ctx)`. Capped at 5 MiB.
    std::optional<std::string> body{};

    ExchangeKind exchange{ExchangeKind::Operation};
};

struct ExtractionApplied {
    RunId runId;
    std::size_t stepIndex{};
    ResourceId resource;
    std::vector<std::string> variableNames;  ///< Names only; auth values masked.
    TimePoint at;
};

/// Per-extraction outcome, one event per extraction declared on an op; mirrors `ExtractionTrace`.
struct ExtractionCompleted {
    enum class Outcome : std::uint8_t { Resolved, Null, Missing, InvalidPattern, Unsupported };

    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    std::string variableName;
    std::string sourcePath;
    Outcome outcome{Outcome::Missing};

    /// Truncated value when `outcome == Resolved`. Empty otherwise.
    std::string value;
    TimePoint at;
};

enum class VariableUseKind : std::uint8_t {
    Unknown,
    UrlPath,
    RawQuery,
    Fragment,
    NamedQuery,
    Header,
    Auth,
    Body,
    FormField,
};

enum class VariableSourceKind : std::uint8_t {
    Unknown,
    Environment,
    Secret,
    Actor,
    Resource,
    Extraction,
};

enum class UnresolvedVariableCause : std::uint8_t {
    Unavailable,
    EnvironmentValueMissing,
    SecretValueMissing,
    ActorSessionFieldMissing,
    ResourceValueMissing,
    ExtractionMissing,
    ExtractionNull,
    ExtractionInvalid,
    ExtractionUnsupported,
};

/// Value-free evidence for one unresolved request variable. `token` excludes template braces;
/// source fields say where a user can navigate; producer fields appear only when the run proves
/// causality.
struct UnresolvedVariableDiagnostic {
    std::string token;
    VariableUseKind useKind{VariableUseKind::Unknown};
    std::string useName;
    UnresolvedVariableCause cause{UnresolvedVariableCause::Unavailable};
    VariableSourceKind sourceKind{VariableSourceKind::Unknown};
    std::string sourceId;
    std::string sourceField;
    std::optional<OperationId> producerOp;
    std::optional<std::size_t> producerStepIndex;
};

struct StepFailed {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    ErrorCode code{};
    ErrorClass cls{};
    int attempt{1};
    std::string detail;
    TimePoint at;
    std::vector<UnresolvedVariableDiagnostic> diagnostics;
};

/// A step that did not run because a prior step failed.
struct StepBlocked {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    std::size_t blockedByStepIndex{};
    TimePoint at;
};

/// One declared assertion evaluated against the final response, streamed per assertion.
struct AssertionCompleted {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    std::string name;
    std::string expr;
    bool passed{false};
    TimePoint at;

    /// Why it failed (see `AssertionResult::detail`); empty when it passed.
    std::string detail{};
};

struct StepCancelled {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    TimePoint at;
};

struct SessionRefreshed {
    enum class Trigger : std::uint8_t { Expiry, Unauthorized };

    RunId runId;
    ActorId actor;
    Trigger trigger{};
    TimePoint at;
};

enum class RunOutcome : std::uint8_t { Succeeded, Failed, Cancelled };

struct RunEnded {
    RunId runId;
    RunOutcome outcome{};
    std::chrono::milliseconds elapsed{};
    TimePoint at;
};

using RunEvent = std::variant<RunStarted,
                              StepStarted,
                              StepSkipped,
                              RequestPrepared,
                              ResponseReceived,
                              ExtractionApplied,
                              ExtractionCompleted,
                              AssertionCompleted,
                              StepFailed,
                              StepBlocked,
                              StepCancelled,
                              SessionRefreshed,
                              RunEnded>;

}  // namespace reqloom::engine
