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

/// Replaces redacted header and extraction values in events. Stable
/// across releases so renderers, persistence, and tests can match on it.
inline constexpr std::string_view kRedactedHeaderValue = "***REDACTED***";

struct RunStarted {
    RunId runId;
    OperationId target;
    std::size_t chainSize{};
    std::string envName;
    TimePoint at;
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

struct RequestPrepared {
    RunId runId;
    std::size_t stepIndex{};
    HttpMethod method{};
    std::string url;  ///< Sanitized display URL; authority/components opaque, not replayable.
    std::vector<std::pair<std::string, std::string>> maskedHeaders;
    std::size_t bodySize{};
    TimePoint at;
};

struct ResponseReceived {
    RunId runId;
    std::size_t stepIndex{};
    int status{};
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t bodySize{};
    std::chrono::milliseconds elapsed{};
    TimePoint at;

    /// Raw response body, populated only when the run opted in via
    /// `RunOptions::captureResponseBodies`. Empty by default — the
    /// engine's redaction-first contract keeps bodies off the event
    /// surface unless a caller explicitly asks for them. When opted in,
    /// every response is captured, including auth/login/refresh bodies
    /// (which carry tokens), so a developer can fully debug each call.
    /// Capped at 5 MiB. Persisted to the history store when present, so
    /// past responses replay in full (Postman-style history).
    std::optional<std::string> body{};
};

struct ExtractionApplied {
    RunId runId;
    std::size_t stepIndex{};
    ResourceId resource;
    std::vector<std::string> variableNames;  ///< Names only; auth values masked.
    TimePoint at;
};

/// Per-extraction outcome — one event per extraction declared on an op.
/// Mirrors `ExtractionTrace` on `RunContext`. Lets the desktop timeline
/// surface resolved values, nulls, and missing fields per step instead
/// of the coarse-grained `ExtractionApplied` summary.
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

/// Safe, value-free evidence for one unresolved request variable.
///
/// Display strings are bounded and escaped before reaching this event. `token`
/// excludes template braces. Source fields identify where a user can navigate;
/// producer fields are present only when the current run proves causality.
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

/// One declared assertion evaluated against the final response. Streamed per
/// assertion (like ExtractionCompleted) so the timeline shows pass/fail rows.
struct AssertionCompleted {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    std::string name;
    std::string expr;
    bool passed{false};
    TimePoint at;
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
