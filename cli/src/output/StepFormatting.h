// Small, side-effect-free helpers shared by every CLI renderer.
// Keeps the per-step glyph and error-code spelling consistent between
// the live event stream, the post-run summary, and the JUnit / JSON
// emitters.

#pragma once

#include <chainapi/engine/PublicApi.h>

#include <string>
#include <string_view>

namespace chainapi::cli {

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

}  // namespace chainapi::cli
