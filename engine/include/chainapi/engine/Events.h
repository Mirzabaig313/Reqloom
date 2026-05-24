// Observability events emitted by the engine (Engine Requirement §10).
// Consumed by the desktop timeline UI and the CLI's renderers.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace chainapi::engine {

struct RunId {
    std::uint64_t value{0};
    auto operator<=>(const RunId&) const = default;
};

using TimePoint = std::chrono::system_clock::time_point;

enum class SkipReason { SessionValid, ExtractionCached };

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
    std::string url;  ///< Fully-resolved.
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
};

struct ExtractionApplied {
    RunId runId;
    std::size_t stepIndex{};
    ResourceId resource;
    std::vector<std::string> variableNames;  ///< Names only; auth values masked.
    TimePoint at;
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
};

struct StepCancelled {
    RunId runId;
    std::size_t stepIndex{};
    OperationId op;
    TimePoint at;
};

struct SessionRefreshed {
    enum class Trigger { Expiry, Unauthorized };

    RunId runId;
    ActorId actor;
    Trigger trigger{};
    TimePoint at;
};

enum class RunOutcome { Succeeded, Failed, Cancelled };

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
                              StepFailed,
                              StepCancelled,
                              SessionRefreshed,
                              RunEnded>;

}  // namespace chainapi::engine
