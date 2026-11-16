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

// C++23 <print> has no std::ostream overload (libc++ rejects it), so format with std::format
// and stream the result with a trailing newline.
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
    // The summary prints even in quiet mode: it is the canonical record of which steps ran.
    ostreamPrintln(summary_, "\n--- Chain Summary ---");
    ostreamPrintln(summary_,
                   "Target: {}   Env: {}   Outcome: {}",
                   target.value,
                   environment,
                   std::string(runOutcomeName(result.outcome)));

    for (const auto& step : result.steps) {
        if (step.pollAttempt || step.forEachIndex) {
            printChildRow(step);
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
        printAssertions(step, "");
    }
}

void TextRenderer::printChildRow(const ce::StepResult& row) {
    std::string label;
    if (row.forEachIndex) {
        label = std::format("iter #{:<2}", *row.forEachIndex);
    }
    if (row.pollAttempt) {
        label = row.forEachIndex
                    ? std::format("iter #{} poll #{:<2}", *row.forEachIndex, *row.pollAttempt)
                    : std::format("poll #{:<2}", *row.pollAttempt);
    }
    ostreamPrintln(summary_,
                   "    {} {:<6} {} ({}ms){}",
                   label,
                   std::string(statusGlyph(row.status)),
                   row.op.value,
                   row.elapsed.count(),
                   row.detail.empty() ? std::string{} : "  " + row.detail);
    if (row.forEachIndex && !row.pollAttempt) {
        printAssertions(row, std::format(" (iter #{})", *row.forEachIndex));
    }
}

void TextRenderer::printAssertions(const ce::StepResult& row, std::string_view attribution) {
    for (const auto& a : row.assertions) {
        ostreamPrintln(summary_,
                       "         {} assert{}: {}",
                       a.passed ? std::string{"\u2713"} : std::string{"\u2717"},
                       attribution,
                       a.name);
        if (!a.passed && !a.detail.empty()) {
            ostreamPrintln(summary_, "             {}", a.detail);
        }
    }
}

}  // namespace reqloom::cli
