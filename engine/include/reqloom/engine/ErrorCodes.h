// Error taxonomy. Codes are stable; UI/CLI render them; QA asserts on them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

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
    /// The TCP connection (or proxy tunnel) could not be established, so no request bytes
    /// reached the server. Retryable for any method.
    NetworkConnect,
    NetworkTls,
    /// Refused by a transport safety rule (non-http(s) scheme, body over
    /// `TransportConfig::maxResponseBytes`, too many redirects); `detail` says which. Not
    /// retryable.
    NetworkPolicy,
    /// A local file referenced by an `@/path` value in `body_form` could not be read (missing, not
    /// regular, or over the size cap). Raised before any HTTP call.
    UploadFileUnreadable,

    // HTTP
    Http5xx,
    Http4xx,
    StatusMismatch,

    // Auth
    SessionRefreshFailed,

    // Secrets
    /// A `{{secret.X}}` could not be loaded from the backend (keychain error, not a missing key; a
    /// missing key just leaves the reference unresolved).
    SecretAccessFailed,

    // Hooks
    HookFailure,
    HookTimeout,

    // Extraction
    ExtractionFailed,
    ResponseParse,

    /// A declared response assertion (`assert:`) evaluated to false against
    /// the final response. Carries the failing expression in `detail`.
    AssertionFailed,

    // Polling
    PollTimeout,              ///< wall-clock budget exceeded
    PollMaxAttemptsExceeded,  ///< attempt-count budget exceeded
    PollFailPredicate,        ///< fail_when matched a poll response

    // LLM (AI importer)
    LlmRequestFailed,    ///< provider call failed (network / 4xx / 5xx)
    LlmResponseInvalid,  ///< provider responded but the body could not be decoded

    // Run
    Cancelled,

    /// An engine-internal precondition was violated (e.g. a read API on an engine without a history
    /// store).
    Internal,
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

/// Stable code string (e.g. "E_CYCLE"), safe in logs.
[[nodiscard]] std::string_view toCodeString(ErrorCode code) noexcept;

/// A short, action-oriented phrase for an error code (e.g. "Authentication failed"); the canonical
/// source of friendly messages for UI/CLI.
[[nodiscard]] std::string_view humanize(ErrorCode code) noexcept;

/// Inverse of `toCodeString`; nullopt for an unrecognised string (e.g. a code from a newer build).
[[nodiscard]] std::optional<ErrorCode> fromCodeString(std::string_view code) noexcept;

/// Whether a step that fails with this code should be retried per the
/// per-operation RetryPolicy.
[[nodiscard]] bool isRetryable(ErrorCode code) noexcept;

[[nodiscard]] ErrorClass classify(ErrorCode code) noexcept;

/// Application-layer error type; infrastructure and application layers return `std::expected<T,
/// ReqloomError>` rather than throwing. `detail` carries human-readable context.
struct ReqloomError {
    ErrorCode code{ErrorCode::SchemaInvalid};
    ErrorClass cls{ErrorClass::Schema};
    std::string detail;
};

}  // namespace reqloom::engine
