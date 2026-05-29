// Formatting — see header. Enum→display-string maps for the UI panels.
#include "Formatting.h"

namespace chainapi::desktop::format {

QString method(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

QString statusGlyph(engine::StepResult::Status status) {
    switch (status) {
        case engine::StepResult::Status::Pending:
            return QStringLiteral("…");
        case engine::StepResult::Status::Ready:
            return QStringLiteral("READY");
        case engine::StepResult::Status::Skipped:
            return QStringLiteral("SKIP");
        case engine::StepResult::Status::Succeeded:
            return QStringLiteral("OK");
        case engine::StepResult::Status::Failed:
            return QStringLiteral("FAIL");
        case engine::StepResult::Status::Cancelled:
            return QStringLiteral("CANC");
        case engine::StepResult::Status::Blocked:
            return QStringLiteral("BLOCK");
    }
    return QStringLiteral("?");
}

QString runOutcome(engine::RunOutcome outcome) {
    switch (outcome) {
        case engine::RunOutcome::Succeeded:
            return QStringLiteral("Succeeded");
        case engine::RunOutcome::Failed:
            return QStringLiteral("Failed");
        case engine::RunOutcome::Cancelled:
            return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString errorCode(engine::ErrorCode code) {
    const auto codeStr = engine::toCodeString(code);
    return QString::fromUtf8(codeStr.data(), static_cast<qsizetype>(codeStr.size()));
}

QString extractionSource(engine::Extraction::Source source) {
    switch (source) {
        case engine::Extraction::Source::JsonPath:
            return QStringLiteral("jsonpath");
        case engine::Extraction::Source::XPath:
            return QStringLiteral("xpath");
        case engine::Extraction::Source::Header:
            return QStringLiteral("header");
        case engine::Extraction::Source::StatusCode:
            return QStringLiteral("status");
        case engine::Extraction::Source::Regex:
            return QStringLiteral("regex");
        case engine::Extraction::Source::Cookie:
            return QStringLiteral("cookie");
    }
    return QStringLiteral("jsonpath");
}

QString extractionOutcome(engine::ExtractionCompleted::Outcome outcome) {
    switch (outcome) {
        case engine::ExtractionCompleted::Outcome::Resolved:
            return QStringLiteral("resolved");
        case engine::ExtractionCompleted::Outcome::Null:
            return QStringLiteral("null");
        case engine::ExtractionCompleted::Outcome::Missing:
            return QStringLiteral("missing");
        case engine::ExtractionCompleted::Outcome::InvalidPattern:
            return QStringLiteral("invalid pattern");
        case engine::ExtractionCompleted::Outcome::Unsupported:
            return QStringLiteral("unsupported");
    }
    return QStringLiteral("missing");
}

QString skipReason(engine::SkipReason reason) {
    switch (reason) {
        case engine::SkipReason::SessionValid:
            return QStringLiteral("session valid");
        case engine::SkipReason::ExtractionCached:
            return QStringLiteral("extraction cached");
    }
    return QStringLiteral("cached");
}

}  // namespace chainapi::desktop::format
