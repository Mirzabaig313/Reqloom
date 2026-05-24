// Stable mapping of ErrorCode → string code, retryability, and class.
// Engine Requirement §5.
#include <chainapi/engine/ErrorCodes.h>

namespace chainapi::engine {

std::string_view toCodeString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SchemaInvalid:        return "E_SCHEMA_INVALID";
        case ErrorCode::YamlParse:            return "E_YAML_PARSE";
        case ErrorCode::Cycle:                return "E_CYCLE";
        case ErrorCode::RefUndefined:         return "E_REF_UNDEFINED";
        case ErrorCode::SchemaVersion:        return "E_SCHEMA_VERSION";
        case ErrorCode::VarUnresolved:        return "E_VAR_UNRESOLVED";
        case ErrorCode::IndexedRefOutOfRange: return "E_INDEXED_REF_OUT_OF_RANGE";
        case ErrorCode::NetworkTimeout:       return "E_NETWORK_TIMEOUT";
        case ErrorCode::NetworkDns:           return "E_NETWORK_DNS";
        case ErrorCode::NetworkTls:           return "E_NETWORK_TLS";
        case ErrorCode::Http5xx:              return "E_HTTP_5XX";
        case ErrorCode::Http4xx:              return "E_HTTP_4XX";
        case ErrorCode::StatusMismatch:       return "E_STATUS_MISMATCH";
        case ErrorCode::SessionRefreshFailed: return "E_SESSION_REFRESH_FAILED";
        case ErrorCode::HookFailure:          return "E_HOOK_FAILURE";
        case ErrorCode::HookTimeout:          return "E_HOOK_TIMEOUT";
        case ErrorCode::ExtractionFailed:     return "E_EXTRACTION_FAILED";
        case ErrorCode::ResponseParse:        return "E_RESPONSE_PARSE";
        case ErrorCode::Cancelled:            return "E_CANCELLED";
    }
    return "E_UNKNOWN";
}

bool isRetryable(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::NetworkTimeout:
        case ErrorCode::NetworkDns:
        case ErrorCode::Http5xx:
            return true;
        default:
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

        case ErrorCode::Http5xx:
        case ErrorCode::Http4xx:
        case ErrorCode::StatusMismatch:
            return ErrorClass::Http;

        case ErrorCode::SessionRefreshFailed:
            return ErrorClass::Auth;

        case ErrorCode::HookFailure:
        case ErrorCode::HookTimeout:
            return ErrorClass::Hook;

        case ErrorCode::ExtractionFailed:
        case ErrorCode::ResponseParse:
            return ErrorClass::Extraction;

        case ErrorCode::Cancelled:
            return ErrorClass::Run;
    }
    return ErrorClass::Run;
}

}  // namespace chainapi::engine
