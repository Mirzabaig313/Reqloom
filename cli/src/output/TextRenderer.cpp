// TextRenderer — see header. Default human-readable format for `chainapi run`.

#include "TextRenderer.h"

#include "StepFormatting.h"

#include <print>
#include <type_traits>
#include <utility>
#include <variant>

namespace chainapi::cli {

namespace {

namespace ce = chainapi::engine;

}  // namespace

TextRenderer::TextRenderer(std::ostream& summaryOut,
                           std::ostream& progressOut,
                           std::ostream& errOut,
                           bool quiet)
    : summary_(summaryOut), progress_(progressOut), err_(errOut), quiet_(quiet) {}

void TextRenderer::printProjectPreamble(const std::string& projectName,
                                        std::size_t actorCount,
                                        std::size_t resourceCount) {
    if (quiet_) return;
    std::println(progress_,
                 "Loaded project: {} ({} actors, {} resources)",
                 projectName,
                 actorCount,
                 resourceCount);
}

void TextRenderer::onEvent(const ce::RunEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ce::RunStarted>) {
                if (quiet_) return;
                std::println(progress_,
                             "Running: {} (chain of {} steps, env={})",
                             e.target.value,
                             e.chainSize,
                             e.envName);
            } else if constexpr (std::is_same_v<T, ce::StepStarted>) {
                if (quiet_) return;
                std::println(progress_,
                             "  [{}] Running: {} (attempt {})",
                             e.stepIndex + 1,
                             e.op.value,
                             e.attempt);
            } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
                if (quiet_) return;
                std::println(progress_, "  [{}] Skipped: {} (cached)", e.stepIndex + 1, e.op.value);
            } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
                // Failures always go to stderr, even when quiet — CI logs need them.
                std::println(err_,
                             "  [{}] FAILED: {} [{}] — {}",
                             e.stepIndex + 1,
                             e.op.value,
                             std::string(ce::toCodeString(e.code)),
                             e.detail);
            } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
                if (quiet_) return;
                std::println(progress_, "\nResult: {}", std::string(runOutcomeName(e.outcome)));
            }
        },
        event);
}

void TextRenderer::render(const ce::OperationId& target,
                          std::string_view environment,
                          const ce::RunResult& result) {
    // Summary table prints even in quiet mode — it's the canonical record
    // of which steps ran. Suppressing it would make the run silent on
    // success, which defeats `--quiet` consumers that do their own parsing.
    std::println(summary_, "\n--- Chain Summary ---");
    std::println(summary_,
                 "Target: {}   Env: {}   Outcome: {}",
                 target.value,
                 environment.empty() ? std::string_view{"<default>"} : environment,
                 std::string(runOutcomeName(result.outcome)));

    for (const auto& step : result.steps) {
        if (step.pollAttempt) {
            std::println(summary_,
                         "    poll #{:<2} {:<6} {} ({}ms){}",
                         *step.pollAttempt,
                         std::string(statusGlyph(step.status)),
                         step.op.value,
                         step.elapsed.count(),
                         step.detail.empty() ? std::string{} : "  " + step.detail);
            continue;
        }
        std::println(summary_,
                     "  {:<6} {} ({}ms) err={}",
                     std::string(statusGlyph(step.status)),
                     step.op.value,
                     step.elapsed.count(),
                     errorCodeName(step));
        if (!step.detail.empty()) {
            std::println(summary_, "         {}", step.detail);
        }
    }
}

}  // namespace chainapi::cli
