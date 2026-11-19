// JsonRenderer — see header. Hand-rolled JSON: the CLI links only the engine and Qt6::Core,
// and the schema is small and stable.

#include "JsonRenderer.h"

#include "NextAction.h"
#include "StepFormatting.h"
#include "TextEncoding.h"

#include <reqloom/engine/PublicApi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;

/// Step totals; poll attempts and for_each iterations are counted apart from the steps they
/// belong to.
struct Summary {
    std::uint32_t succeeded{0};
    std::uint32_t failed{0};
    std::uint32_t skipped{0};
    std::uint32_t blocked{0};
    std::uint32_t cancelled{0};
    std::uint32_t pollAttempts{0};
    std::uint32_t forEachIterations{0};
};

[[nodiscard]] Summary summarize(const ce::RunResult& result) {
    Summary s;
    for (const auto& step : result.steps) {
        if (step.pollAttempt) {
            ++s.pollAttempts;
            continue;
        }
        if (step.forEachIndex) {
            ++s.forEachIterations;
            continue;
        }
        switch (step.status) {
            case ce::StepResult::Status::Succeeded:
                ++s.succeeded;
                break;
            case ce::StepResult::Status::Failed:
                ++s.failed;
                break;
            case ce::StepResult::Status::Skipped:
                ++s.skipped;
                break;
            case ce::StepResult::Status::Blocked:
                ++s.blocked;
                break;
            case ce::StepResult::Status::Cancelled:
                ++s.cancelled;
                break;
            default:
                break;
        }
    }
    return s;
}

/// A JSON string literal; text is escaped in one linear pass, so a detail of a MiB or more is fine.
[[nodiscard]] std::string jsonString(std::string_view text) {
    return "\"" + jsonEscape(text) + "\"";
}

/// Everything before `steps`, shared by a run and an error document.
void writeHead(std::ostream& out,
               std::string_view runId,
               const ce::OperationId& target,
               std::string_view environment,
               std::string_view outcome,
               const Summary& s) {
    out << "{\n";
    out << "  \"schema_version\": " << JsonRenderer::kSchemaVersion << ",\n";
    out << "  \"contract_version\": " << jsonString(JsonRenderer::kContractVersion) << ",\n";
    out << "  \"run_id\": " << runId << ",\n";
    out << "  \"target\": " << jsonString(target.value) << ",\n";
    out << "  \"environment\": " << jsonString(environment) << ",\n";
    out << "  \"outcome\": " << jsonString(outcome) << ",\n";
    out << "  \"summary\": {\n";
    out << "    \"succeeded\": " << s.succeeded << ",\n";
    out << "    \"failed\": " << s.failed << ",\n";
    out << "    \"skipped\": " << s.skipped << ",\n";
    out << "    \"blocked\": " << s.blocked << ",\n";
    out << "    \"cancelled\": " << s.cancelled << ",\n";
    out << "    \"for_each_iterations\": " << s.forEachIterations << ",\n";
    out << "    \"poll_attempts\": " << s.pollAttempts << "\n";
    out << "  },\n";
}

template <typename T>
[[nodiscard]] std::string orNull(const std::optional<T>& value) {
    return value ? std::to_string(*value) : std::string{"null"};
}

void writeExtracted(std::ostream& out, const ce::StepResult& step) {
    out << "      \"extracted\": [";
    bool first = true;
    for (const auto& value : step.extracted) {
        out << (first ? "" : ",") << "\n        { \"name\": " << jsonString(value.name)
            << ", \"value\": " << jsonString(value.value)
            << ", \"truncated\": " << (value.truncated ? "true" : "false") << " }";
        first = false;
    }
    out << (step.extracted.empty() ? "" : "\n      ") << "],\n";
}

/// The response sits in its own object, marked as content from the system under test, so an
/// agent can tell it from Reqloom's words (FR-15.4). Already masked and cut by the engine.
void writeResponse(std::ostream& out, const ce::StepResult& step) {
    out << "      \"response\": ";
    if (!step.response) {
        out << "null,\n";
        return;
    }
    const auto& r = *step.response;
    out << "{\n";
    out << "        \"source\": \"system_under_test\",\n";
    out << "        \"untrusted\": true,\n";
    out << "        \"body\": " << jsonString(r.body) << ",\n";
    out << "        \"body_bytes\": " << r.totalBytes << ",\n";
    out << "        \"limit_bytes\": " << r.limitBytes << ",\n";
    out << "        \"truncated\": " << (r.truncated ? "true" : "false") << ",\n";
    out << "        \"binary\": " << (r.binary ? "true" : "false") << "\n";
    out << "      },\n";
}

void writeStep(std::ostream& out, const ce::StepResult& step, const ce::OperationId& target) {
    out << "\n    {\n";
    out << "      \"op\": " << jsonString(step.op.value) << ",\n";
    out << "      \"status\": " << jsonString(statusGlyph(step.status)) << ",\n";
    out << "      \"attempts\": " << step.attempts << ",\n";
    out << "      \"elapsed_ms\": " << step.elapsed.count() << ",\n";
    out << "      \"error_code\": "
        << (step.error ? jsonString(ce::toCodeString(*step.error)) : std::string{"null"}) << ",\n";
    out << "      \"poll_attempt\": " << orNull(step.pollAttempt) << ",\n";
    out << "      \"for_each_index\": " << orNull(step.forEachIndex) << ",\n";
    out << "      \"detail\": " << jsonString(step.detail) << ",\n";
    out << "      \"assertions\": [";
    bool first = true;
    for (const auto& a : step.assertions) {
        out << (first ? "" : ",") << "\n        { \"name\": " << jsonString(a.name)
            << ", \"expr\": " << jsonString(a.expr)
            << ", \"passed\": " << (a.passed ? "true" : "false")
            << ", \"detail\": " << jsonString(a.detail) << " }";
        first = false;
    }
    out << (step.assertions.empty() ? "" : "\n      ") << "],\n";
    out << "      \"http_status\": " << orNull(step.httpStatus) << ",\n";
    writeExtracted(out, step);
    writeResponse(out, step);
    out << "      \"next_action\": "
        << (step.status == ce::StepResult::Status::Failed
                ? jsonString(stepNextAction(step, target.value))
                : std::string{"null"})
        << "\n";
    out << "    }";
}

/// Index of the row that failed the run: the first failed operation row, else the first failed
/// poll attempt or for_each iteration.
[[nodiscard]] std::optional<std::size_t> failedStepIndex(const ce::RunResult& result) {
    std::optional<std::size_t> any{};
    for (std::size_t i = 0; i < result.steps.size(); ++i) {
        const auto& step = result.steps[i];
        if (step.status != ce::StepResult::Status::Failed) {
            continue;
        }
        if (!step.pollAttempt && !step.forEachIndex) {
            return i;
        }
        any = any.value_or(i);
    }
    return any;
}

void writeFailedStep(std::ostream& out,
                     const ce::OperationId& target,
                     const ce::RunResult& result) {
    const auto index = failedStepIndex(result);
    if (!index) {
        out << "  \"failed_step\": null,\n";
        return;
    }
    const auto& step = result.steps[*index];
    out << "  \"failed_step\": {\n";
    out << "    \"index\": " << *index << ",\n";
    out << "    \"op\": " << jsonString(step.op.value) << ",\n";
    out << "    \"error_code\": "
        << (step.error ? jsonString(ce::toCodeString(*step.error)) : std::string{"null"}) << ",\n";
    out << "    \"next_action\": " << jsonString(stepNextAction(step, target.value)) << "\n";
    out << "  },\n";
}

}  // namespace

JsonRenderer::JsonRenderer(std::ostream& out) : out_(out) {}

void JsonRenderer::render(const ce::OperationId& target,
                          std::string_view environment,
                          const ce::RunResult& result,
                          const bool recorded) {
    writeHead(out_,
              std::to_string(result.runId.value),
              target,
              environment,
              runOutcomeName(result.outcome),
              summarize(result));
    out_ << "  \"recorded\": " << (recorded ? "true" : "false") << ",\n";
    out_ << "  \"error\": null,\n";
    writeFailedStep(out_, target, result);
    out_ << "  \"steps\": [";
    bool first = true;
    for (const auto& step : result.steps) {
        out_ << (first ? "" : ",");
        writeStep(out_, step, target);
        first = false;
    }
    out_ << (result.steps.empty() ? "" : "\n  ") << "]\n";
    out_ << "}\n";
}

void JsonRenderer::renderError(const ce::OperationId& target,
                               std::string_view environment,
                               std::optional<ce::RunId> run,
                               std::string_view code,
                               std::string_view detail) {
    const std::string runId = run ? std::to_string(run->value) : std::string{"null"};
    writeHead(out_, runId, target, environment, kErrorOutcomeName, Summary{});
    out_ << "  \"recorded\": false,\n";
    out_ << "  \"error\": {\n";
    out_ << "    \"code\": " << jsonString(code) << ",\n";
    out_ << "    \"detail\": " << jsonString(detail) << ",\n";
    out_ << "    \"next_action\": " << jsonString(errorNextAction(code)) << "\n";
    out_ << "  },\n";
    out_ << "  \"failed_step\": null,\n";
    out_ << "  \"steps\": []\n";
    out_ << "}\n";
}

}  // namespace reqloom::cli
