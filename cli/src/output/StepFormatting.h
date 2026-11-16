// Side-effect-free helpers shared by every CLI renderer, keeping glyphs and error-code spelling
// consistent.

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <string>
#include <string_view>

namespace reqloom::cli {

[[nodiscard]] constexpr std::string_view statusGlyph(engine::StepResult::Status status) noexcept {
    using S = engine::StepResult::Status;
    switch (status) {
        case S::Succeeded:
            return "OK";
        case S::Skipped:
            return "SK";
        case S::Failed:
            return "FAIL";
        case S::Cancelled:
            return "CANCEL";
        case S::Blocked:
            return "BLOCK";
        case S::Pending:
            return "PEND";
        case S::Ready:
            return "READY";
    }
    return "?";
}

[[nodiscard]] inline std::string errorCodeName(const engine::StepResult& step) {
    if (!step.error) {
        return "—";
    }
    return std::string(engine::toCodeString(*step.error));
}

/// `outcome` of a machine-readable document for a run that ended before its first step.
inline constexpr std::string_view kErrorOutcomeName = "ERROR";

[[nodiscard]] constexpr std::string_view runOutcomeName(engine::RunOutcome outcome) noexcept {
    switch (outcome) {
        case engine::RunOutcome::Succeeded:
            return "SUCCEEDED";
        case engine::RunOutcome::Failed:
            return "FAILED";
        case engine::RunOutcome::Cancelled:
            return "CANCELLED";
    }
    return "UNKNOWN";
}

}  // namespace reqloom::cli
