// Error taxonomy. Codes are stable; UI/CLI render them; QA asserts on them.
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

    // Polling
    PollTimeout,              ///< wall-clock budget exceeded
    PollMaxAttemptsExceeded,  ///< attempt-count budget exceeded
    PollFailPredicate,        ///< fail_when matched a poll response

    // LLM (AI importer)
    LlmRequestFailed,    ///< provider call failed (network / 4xx / 5xx)
    LlmResponseInvalid,  ///< provider responded but the body could not be decoded

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
    Llm,
    Run,
};

/// Stable, human-readable code string (e.g. "E_CYCLE"). Safe in logs and
/// asserted on by integration tests.
std::string_view toCodeString(ErrorCode code) noexcept;

/// Whether a step that fails with this code should be retried per the
/// per-operation RetryPolicy.
bool isRetryable(ErrorCode code) noexcept;

ErrorClass classify(ErrorCode code) noexcept;

/// Application-layer error type. Infrastructure and application layers
/// return `std::expected<T, ChainApiError>` rather than throwing.
/// `ErrorCode` is the stable identifier; `detail` carries human-readable
/// context (file:line, response excerpt, etc.).
struct ChainApiError {
    ErrorCode code{ErrorCode::SchemaInvalid};
    ErrorClass cls{ErrorClass::Schema};
    std::string detail;
};

}  // namespace chainapi::engine
