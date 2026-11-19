// How `reqloom run` records itself in the project's run history (PRD FR-16.3, FR-15.8) so that
// `reqloom show` can print a step later and the desktop app lists the run.
//
// What is recorded for `show` is the run's step rows after the engine's agent masking: the same
// masking `run --format json` gets, applied with every secret the run learned, before anything is
// stored. The events the desktop shows are recorded beside them as the engine emits them.
#pragma once

#include "../output/TextEncoding.h"

#include <reqloom/engine/PublicApi.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// Most bytes of masked response (and request) body recorded per step for `show`, unless
/// `--response-body-size` asks for more (64 KiB).
inline constexpr std::size_t kRecordedBodyBytes = std::size_t{64} * 1024;

/// Environment variable an agent sets to name itself on the runs it starts (`--agent` wins).
inline constexpr const char* kAgentVariable = "REQLOOM_AGENT";

/// Longest agent name kept.
inline constexpr std::size_t kMaxAgentNameBytes = 64;

/// The initiator for an agent name given with `--agent` or `REQLOOM_AGENT` (empty: a person at the
/// command line), or why the name is refused: 1–64 characters of letters, digits, space, `.`, `_`,
/// `-`, `/` and `@`, so it cannot carry terminal control sequences into the app or a later
/// listing. Self-reported and for display only (FR-15.8).
[[nodiscard]] inline std::expected<engine::RunInitiator, std::string> initiatorFor(
    std::string_view agentName) {
    if (agentName.empty()) {
        return engine::RunInitiator{engine::RunInitiator::Kind::Cli, {}};
    }
    if (agentName.size() > kMaxAgentNameBytes) {
        return std::unexpected(std::string{"the agent name is longer than 64 characters"});
    }
    for (const char c : agentName) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        std::string_view{" ._-/@"}.find(c) != std::string_view::npos;
        if (!ok) {
            return std::unexpected(
                std::string{"the agent name may hold only letters, digits, space and . _ - / @"});
        }
    }
    return engine::RunInitiator{engine::RunInitiator::Kind::Agent, std::string{agentName}};
}

/// `text` cut to at most `limit` bytes without splitting a UTF-8 sequence.
[[nodiscard]] inline std::string cutUtf8(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string{text};
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return std::string{text.substr(0, end)};
}

/// `body` (already masked) shown at most `limit` bytes: the cut is recorded on it as the engine
/// records its own, so `limit_bytes` and `truncated` describe what is printed.
inline void limitBody(engine::StepResponse& body, std::size_t limit) {
    const bool cut = body.body.size() > limit;
    body.body = cutUtf8(body.body, limit);
    body.limitBytes = limit;
    body.truncated = body.truncated || cut;
}

/// The run's rows with each response cut to `limit` for `run --format json`; the run recorded
/// more for `show`. Cutting a masked body leaves it masked.
[[nodiscard]] inline engine::RunResult withResponseLimit(engine::RunResult result,
                                                         std::size_t limit) {
    for (auto& step : result.steps) {
        if (step.response) {
            limitBody(*step.response, limit);
        }
        step.request.reset();
    }
    return result;
}

}  // namespace reqloom::cli
