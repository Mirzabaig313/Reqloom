// Error taxonomy from Engine Requirement §5. Codes are stable; UI/CLI
// render them; QA asserts on them.
#pragma once

#include <string>
#include <string_view>

namespace chainapi::engine {

enum class ErrorCode {
    // Schema layer
    SchemaInvalid,
    YamlParse,
    Cycle,
    RefUndefined,
    SchemaVersion,

    // Variable resolution
    VarUnresolved,
    IndexedRefOutOfRange,

    // Network
    NetworkTimeout,
    NetworkDns,
    NetworkTls,

    // HTTP
    Http5xx,
    Http4xx,
    StatusMismatch,

    // Auth
    SessionRefreshFailed,

    // Hooks
    HookFailure,
    HookTimeout,

    // Extraction
    ExtractionFailed,
    ResponseParse,

    // Polling (PRD §5.11)
    PollTimeout,             ///< wall-clock budget exceeded
    PollMaxAttemptsExceeded, ///< attempt-count budget exceeded
    PollFailPredicate,       ///< fail_when matched a poll response

    // Run
    Cancelled,
};

enum class ErrorClass {
    Schema,
    Resolution,
    Network,
    Http,
    Auth,
    Hook,
    Extraction,
    Polling,
    Run,
};

/// Stable, human-readable code string (e.g. "E_CYCLE"). Safe in logs and
/// asserted on by integration tests.
std::string_view toCodeString(ErrorCode code) noexcept;

/// Whether a step that fails with this code should be retried per the
/// per-operation RetryPolicy. Engine spec §3.5.
bool isRetryable(ErrorCode code) noexcept;

ErrorClass classify(ErrorCode code) noexcept;

/// Application-layer error type. The infrastructure and application layers
/// return `std::expected<T, ChainApiError>` rather than throwing. Engine
/// spec §5 treats `ErrorCode` as the stable identifier; `detail` carries
/// human-readable context (file:line, response excerpt, etc.).
struct ChainApiError {
    ErrorCode code{ErrorCode::SchemaInvalid};
    ErrorClass cls{ErrorClass::Schema};
    std::string detail;
};

}  // namespace chainapi::engine
