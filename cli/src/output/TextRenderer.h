// Human-readable run renderer. Default format for `chainapi run`.

#pragma once

#include <chainapi/engine/PublicApi.h>

#include <ostream>
#include <string>

namespace chainapi::cli {

/// Renders a run as plain text. Emits live progress on each `RunEvent`
/// (suppressed when constructed with `quiet=true`) and a chain summary
/// table on `render()`. Failures (`StepFailed`) always go to `errOut`
/// even in quiet mode so CI logs surface the cause.
///
/// Live progress goes to `progressOut`; the post-run chain summary goes
/// to `summaryOut`. The two are separate so `--output some.txt` can
/// redirect the summary while live progress keeps streaming to stdout.
class TextRenderer {
public:
    TextRenderer(std::ostream& summaryOut,
                 std::ostream& progressOut,
                 std::ostream& errOut,
                 bool quiet);

    /// Print one-shot project preamble (project name, actor / resource counts).
    void printProjectPreamble(const std::string& projectName,
                              std::size_t actorCount,
                              std::size_t resourceCount);

    /// Stream-style event handler — wire as `engine::ExecutionEngine::EventCallback`.
    void onEvent(const engine::RunEvent& event);

    /// Print the chain summary table after the run completes.
    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

private:
    std::ostream& summary_;
    std::ostream& progress_;
    std::ostream& err_;
    bool quiet_;
};

}  // namespace chainapi::cli
