// Error taxonomy. Codes are stable; UI/CLI render them; QA asserts on them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chainapi::engine {

enum class ErrorCode : std::uint8_t {
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
    /// Local file referenced by an `@/path` value in `body_form` could
    /// not be read (missing, not a regular file, or exceeded the size
    /// cap). Surfaced from the executor before any HTTP call is made.
    UploadFileUnreadable,

    // HTTP
    Http5xx,
    Http4xx,
    StatusMismatch,

    // Auth
    SessionRefreshFailed,

    // Secrets
    /// A `{{secret.X}}` reference could not be loaded from the secret
    /// store backend (keychain error, not a missing key). A missing key
    /// is not an error here — it simply leaves the reference unresolved,
    /// which surfaces later as `VarUnresolved` at request-build time.
    SecretAccessFailed,

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

enum class ErrorClass : std::uint8_t {
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
[[nodiscard]] std::string_view toCodeString(ErrorCode code) noexcept;

/// Inverse of `toCodeString`. Returns the matching code, or nullopt for
/// an unrecognised string (e.g. a code persisted by a newer build).
/// Callers decide the fallback — the history store maps nullopt to
/// `SchemaInvalid` so an unknown persisted code still surfaces.
[[nodiscard]] std::optional<ErrorCode> fromCodeString(std::string_view code) noexcept;

/// Whether a step that fails with this code should be retried per the
/// per-operation RetryPolicy.
[[nodiscard]] bool isRetryable(ErrorCode code) noexcept;

[[nodiscard]] ErrorClass classify(ErrorCode code) noexcept;

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
