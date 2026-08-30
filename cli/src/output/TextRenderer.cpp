// TextRenderer — see header. Default human-readable format for `reqloom run`.

#include "TextRenderer.h"

#include "StepFormatting.h"

#include <format>
#include <ostream>
#include <type_traits>
#include <utility>
#include <variant>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;

// C++23 <print> only provides std::println(FILE*, ...) and std::println(...)
// overloads — no std::ostream overload. Xcode 15.4's libc++ correctly rejects
// std::println(ostream, ...) per the standard, so we format via std::format
// and stream the result with a trailing newline (println semantics).
template <typename... Args>
void ostreamPrintln(std::ostream& out, std::format_string<Args...> fmt, Args&&... args) {
    out << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

}  // namespace

TextRenderer::TextRenderer(std::ostream& summaryOut,
                           std::ostream& progressOut,
                           std::ostream& errOut,
                           bool quiet)
    : summary_(summaryOut), progress_(progressOut), err_(errOut), quiet_(quiet) {}

void TextRenderer::printProjectPreamble(const std::string& projectName,
                                        std::size_t actorCount,
                                        std::size_t resourceCount) {
    if (quiet_) {
        return;
    }
    ostreamPrintln(progress_,
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
                if (quiet_) {
                    return;
                }
                ostreamPrintln(progress_,
                               "Running: {} (chain of {} steps, env={})",
                               e.target.value,
                               e.chainSize,
                               e.envName);
            } else if constexpr (std::is_same_v<T, ce::StepStarted>) {
                if (quiet_) {
                    return;
                }
                ostreamPrintln(progress_,
                               "  [{}] Running: {} (attempt {})",
                               e.stepIndex + 1,
                               e.op.value,
                               e.attempt);
            } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
                if (quiet_) {
                    return;
                }
                ostreamPrintln(
                    progress_, "  [{}] Skipped: {} (cached)", e.stepIndex + 1, e.op.value);
            } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
                // Failures always go to stderr, even when quiet — CI logs need them.
                ostreamPrintln(err_,
                               "  [{}] FAILED: {} [{}] — {}",
                               e.stepIndex + 1,
                               e.op.value,
                               std::string(ce::toCodeString(e.code)),
                               e.detail);
            } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
                if (quiet_) {
                    return;
                }
                ostreamPrintln(progress_, "\nResult: {}", std::string(runOutcomeName(e.outcome)));
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
    ostreamPrintln(summary_, "\n--- Chain Summary ---");
    ostreamPrintln(summary_,
                   "Target: {}   Env: {}   Outcome: {}",
                   target.value,
                   environment.empty() ? std::string_view{"<default>"} : environment,
                   std::string(runOutcomeName(result.outcome)));

    for (const auto& step : result.steps) {
        if (step.pollAttempt) {
            ostreamPrintln(summary_,
                           "    poll #{:<2} {:<6} {} ({}ms){}",
                           *step.pollAttempt,
                           std::string(statusGlyph(step.status)),
                           step.op.value,
                           step.elapsed.count(),
                           step.detail.empty() ? std::string{} : "  " + step.detail);
            continue;
        }
        if (step.forEachIndex) {
            ostreamPrintln(summary_,
                           "    iter #{:<2} {:<6} {} ({}ms){}",
                           *step.forEachIndex,
                           std::string(statusGlyph(step.status)),
                           step.op.value,
                           step.elapsed.count(),
                           step.detail.empty() ? std::string{} : "  " + step.detail);
            continue;
        }
        ostreamPrintln(summary_,
                       "  {:<6} {} ({}ms) err={}",
                       std::string(statusGlyph(step.status)),
                       step.op.value,
                       step.elapsed.count(),
                       errorCodeName(step));
        if (!step.detail.empty()) {
            ostreamPrintln(summary_, "         {}", step.detail);
        }
        for (const auto& a : step.assertions) {
            ostreamPrintln(summary_,
                           "         {} assert: {}",
                           a.passed ? std::string{"\u2713"} : std::string{"\u2717"},
                           a.name);
        }
    }
}

}  // namespace reqloom::cli
