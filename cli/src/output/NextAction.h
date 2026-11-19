// NextAction — the one-line `next_action` of a machine-readable failure, written for an AI agent
// (PRD FR-13.11). Built only from Reqloom's own data (error code, operation id, HTTP status),
// never from a step's `detail` or a response body, so text the API returned cannot pass as an
// instruction from Reqloom (FR-15.4).

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <string>
#include <string_view>

namespace reqloom::cli {

/// What to do about `step`, a failed row of the run whose target is `target`.
[[nodiscard]] std::string stepNextAction(const engine::StepResult& step, std::string_view target);

/// What to do about an error document's `code` (a run that ended before its first step, or a
/// command that could not answer). `command` names the command in the advice.
[[nodiscard]] std::string errorNextAction(std::string_view code, std::string_view command = "run");

}  // namespace reqloom::cli
