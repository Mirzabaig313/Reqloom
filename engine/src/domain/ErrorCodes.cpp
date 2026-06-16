// Stable mapping of ErrorCode → string code, retryability, and class.
#include <reqloom/engine/ErrorCodes.h>

#include <array>
#include <optional>

namespace reqloom::engine {

std::string_view toCodeString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SchemaInvalid:
            return "E_SCHEMA_INVALID";
        case ErrorCode::YamlParse:
            return "E_YAML_PARSE";
        case ErrorCode::Cycle:
            return "E_CYCLE";
        case ErrorCode::RefUndefined:
            return "E_REF_UNDEFINED";
        case ErrorCode::SchemaVersion:
            return "E_SCHEMA_VERSION";
        case ErrorCode::VarUnresolved:
            return "E_VAR_UNRESOLVED";
        case ErrorCode::IndexedRefOutOfRange:
            return "E_INDEXED_REF_OUT_OF_RANGE";
        case ErrorCode::NetworkTimeout:
            return "E_NETWORK_TIMEOUT";
        case ErrorCode::NetworkDns:
            return "E_NETWORK_DNS";
        case ErrorCode::NetworkTls:
            return "E_NETWORK_TLS";
        case ErrorCode::UploadFileUnreadable:
            return "E_UPLOAD_FILE_UNREADABLE";
        case ErrorCode::Http5xx:
            return "E_HTTP_5XX";
        case ErrorCode::Http4xx:
            return "E_HTTP_4XX";
        case ErrorCode::StatusMismatch:
            return "E_STATUS_MISMATCH";
        case ErrorCode::SessionRefreshFailed:
            return "E_SESSION_REFRESH_FAILED";
        case ErrorCode::SecretAccessFailed:
            return "E_SECRET_ACCESS_FAILED";
        case ErrorCode::HookFailure:
            return "E_HOOK_FAILURE";
        case ErrorCode::HookTimeout:
            return "E_HOOK_TIMEOUT";
        case ErrorCode::ExtractionFailed:
            return "E_EXTRACTION_FAILED";
        case ErrorCode::ResponseParse:
            return "E_RESPONSE_PARSE";
        case ErrorCode::AssertionFailed:
            return "E_ASSERTION_FAILED";
        case ErrorCode::PollTimeout:
            return "E_POLL_TIMEOUT";
        case ErrorCode::PollMaxAttemptsExceeded:
            return "E_POLL_MAX_ATTEMPTS_EXCEEDED";
        case ErrorCode::PollFailPredicate:
            return "E_POLL_FAIL_PREDICATE";
        case ErrorCode::LlmRequestFailed:
            return "E_LLM_REQUEST_FAILED";
        case ErrorCode::LlmResponseInvalid:
            return "E_LLM_RESPONSE_INVALID";
        case ErrorCode::Cancelled:
            return "E_CANCELLED";
    }
    return "E_UNKNOWN";
}

std::string_view humanize(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SchemaInvalid:
            return "Invalid schema";
        case ErrorCode::YamlParse:
            return "Could not parse YAML";
        case ErrorCode::Cycle:
            return "Dependency cycle";
        case ErrorCode::RefUndefined:
            return "Missing reference";
        case ErrorCode::SchemaVersion:
            return "Unsupported schema version";
        case ErrorCode::VarUnresolved:
            return "Unresolved variable";
        case ErrorCode::IndexedRefOutOfRange:
            return "List index out of range";
        case ErrorCode::NetworkTimeout:
            return "Request timed out";
        case ErrorCode::NetworkDns:
            return "Could not resolve host";
        case ErrorCode::NetworkTls:
            return "TLS handshake failed";
        case ErrorCode::UploadFileUnreadable:
            return "Upload file unreadable";
        case ErrorCode::Http5xx:
            return "Server error";
        case ErrorCode::Http4xx:
            return "Request rejected";
        case ErrorCode::StatusMismatch:
            return "Unexpected status code";
        case ErrorCode::SessionRefreshFailed:
            return "Authentication failed";
        case ErrorCode::SecretAccessFailed:
            return "Could not read secret";
        case ErrorCode::HookFailure:
            return "Hook script failed";
        case ErrorCode::HookTimeout:
            return "Hook script timed out";
        case ErrorCode::ExtractionFailed:
            return "Extraction failed";
        case ErrorCode::ResponseParse:
            return "Could not parse response";
        case ErrorCode::AssertionFailed:
            return "Assertion failed";
        case ErrorCode::PollTimeout:
            return "Polling timed out";
        case ErrorCode::PollMaxAttemptsExceeded:
            return "Polling gave up";
        case ErrorCode::PollFailPredicate:
            return "Polling hit a failure condition";
        case ErrorCode::LlmRequestFailed:
            return "AI request failed";
        case ErrorCode::LlmResponseInvalid:
            return "AI response was invalid";
        case ErrorCode::Cancelled:
            return "Cancelled";
    }
    return "Failed";
}

std::optional<ErrorCode> fromCodeString(std::string_view code) noexcept {
    // Reverse of toCodeString, matched against the full enumerator list.
    // The drift guard below fails the build if a code is added without
    // growing this array.
    constexpr std::array<ErrorCode, 27> kAll = {
        ErrorCode::SchemaInvalid,
        ErrorCode::YamlParse,
        ErrorCode::Cycle,
        ErrorCode::RefUndefined,
        ErrorCode::SchemaVersion,
        ErrorCode::VarUnresolved,
        ErrorCode::IndexedRefOutOfRange,
        ErrorCode::NetworkTimeout,
        ErrorCode::NetworkDns,
        ErrorCode::NetworkTls,
        ErrorCode::UploadFileUnreadable,
        ErrorCode::Http5xx,
        ErrorCode::Http4xx,
        ErrorCode::StatusMismatch,
        ErrorCode::SessionRefreshFailed,
        ErrorCode::SecretAccessFailed,
        ErrorCode::HookFailure,
        ErrorCode::HookTimeout,
        ErrorCode::ExtractionFailed,
        ErrorCode::ResponseParse,
        ErrorCode::AssertionFailed,
        ErrorCode::PollTimeout,
        ErrorCode::PollMaxAttemptsExceeded,
        ErrorCode::PollFailPredicate,
        ErrorCode::LlmRequestFailed,
        ErrorCode::LlmResponseInvalid,
        ErrorCode::Cancelled,
    };
    // Cancelled is the last enumerator, so its value + 1 is the count.
    static_assert(static_cast<std::size_t>(ErrorCode::Cancelled) + 1 == kAll.size(),
                  "fromCodeString::kAll is out of sync with the ErrorCode enum");
    for (const auto c : kAll) {
        if (toCodeString(c) == code) {
            return c;
        }
    }
    return std::nullopt;
}

bool isRetryable(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::NetworkTimeout:
        case ErrorCode::NetworkDns:
        case ErrorCode::Http5xx:
            return true;
        default:
            // Poll outcomes are not retryable — the polling loop owns its own retry budget.
            return false;
    }
}

ErrorClass classify(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SchemaInvalid:
        case ErrorCode::YamlParse:
        case ErrorCode::Cycle:
        case ErrorCode::RefUndefined:
        case ErrorCode::SchemaVersion:
            return ErrorClass::Schema;

        case ErrorCode::VarUnresolved:
        case ErrorCode::IndexedRefOutOfRange:
            return ErrorClass::Resolution;

        case ErrorCode::NetworkTimeout:
        case ErrorCode::NetworkDns:
        case ErrorCode::NetworkTls:
            return ErrorClass::Network;

        case ErrorCode::UploadFileUnreadable:
            return ErrorClass::Resolution;

        case ErrorCode::Http5xx:
        case ErrorCode::Http4xx:
        case ErrorCode::StatusMismatch:
            return ErrorClass::Http;

        case ErrorCode::SessionRefreshFailed:
            return ErrorClass::Auth;

        case ErrorCode::SecretAccessFailed:
            return ErrorClass::Auth;

        case ErrorCode::HookFailure:
        case ErrorCode::HookTimeout:
            return ErrorClass::Hook;

        case ErrorCode::ExtractionFailed:
        case ErrorCode::ResponseParse:
        case ErrorCode::AssertionFailed:
            return ErrorClass::Extraction;

        case ErrorCode::PollTimeout:
        case ErrorCode::PollMaxAttemptsExceeded:
        case ErrorCode::PollFailPredicate:
            return ErrorClass::Polling;

        case ErrorCode::LlmRequestFailed:
        case ErrorCode::LlmResponseInvalid:
            return ErrorClass::Llm;

        case ErrorCode::Cancelled:
            return ErrorClass::Run;
    }
    return ErrorClass::Run;
}

}  // namespace reqloom::engine
