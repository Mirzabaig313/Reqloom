// JsonRenderer — see header. Hand-rolled JSON: the CLI links only the engine and Qt6::Core,
// and the schema is small and stable.

#include "JsonRenderer.h"

#include "StepFormatting.h"

#include <reqloom/engine/PublicApi.h>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;

/// Escape per RFC 8259 §7. Linear, so a long detail (a failure excerpt can reach 1 MiB) is safe.
std::string escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 2);
    for (const char ch : input) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    std::ostringstream esc;
                    esc << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(byte);
                    out += esc.str();
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

void writeStatus(std::ostream& out, ce::StepResult::Status status) {
    out << "\"" << statusGlyph(status) << "\"";
}

}  // namespace

JsonRenderer::JsonRenderer(std::ostream& out) : out_(out) {}

void JsonRenderer::render(const ce::OperationId& target,
                          std::string_view environment,
                          const ce::RunResult& result) {
    // Aggregate counts let CI gate on "any failure" without scanning steps.
    std::uint32_t succeeded = 0;
    std::uint32_t failed = 0;
    std::uint32_t skipped = 0;
    std::uint32_t blocked = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t pollAttempts = 0;
    std::uint32_t forEachIterations = 0;
    for (const auto& s : result.steps) {
        if (s.pollAttempt) {
            ++pollAttempts;
            continue;  // poll attempts don't count toward step totals
        }
        if (s.forEachIndex) {
            ++forEachIterations;
            continue;  // for-each iterations group under their parent step
        }
        switch (s.status) {
            case ce::StepResult::Status::Succeeded:
                ++succeeded;
                break;
            case ce::StepResult::Status::Failed:
                ++failed;
                break;
            case ce::StepResult::Status::Skipped:
                ++skipped;
                break;
            case ce::StepResult::Status::Blocked:
                ++blocked;
                break;
            case ce::StepResult::Status::Cancelled:
                ++cancelled;
                break;
            default:
                break;
        }
    }

    out_ << "{\n";
    out_ << "  \"run_id\": " << result.runId.value << ",\n";
    out_ << R"(  "target": ")" << escape(target.value) << "\",\n";
    out_ << R"(  "environment": ")" << escape(environment) << "\",\n";
    out_ << R"(  "outcome": ")" << runOutcomeName(result.outcome) << "\",\n";
    out_ << "  \"summary\": {\n";
    out_ << "    \"succeeded\": " << succeeded << ",\n";
    out_ << "    \"failed\": " << failed << ",\n";
    out_ << "    \"skipped\": " << skipped << ",\n";
    out_ << "    \"blocked\": " << blocked << ",\n";
    out_ << "    \"cancelled\": " << cancelled << ",\n";
    out_ << "    \"for_each_iterations\": " << forEachIterations << ",\n";
    out_ << "    \"poll_attempts\": " << pollAttempts << "\n";
    out_ << "  },\n";

    out_ << "  \"steps\": [";
    bool first = true;
    for (const auto& step : result.steps) {
        if (!first) {
            out_ << ",";
        }
        first = false;
        out_ << "\n    {\n";
        out_ << R"(      "op": ")" << escape(step.op.value) << "\",\n";
        out_ << "      \"status\": ";
        writeStatus(out_, step.status);
        out_ << ",\n";
        out_ << "      \"attempts\": " << step.attempts << ",\n";
        out_ << "      \"elapsed_ms\": " << step.elapsed.count() << ",\n";
        if (step.error) {
            out_ << R"(      "error_code": ")" << escape(ce::toCodeString(*step.error)) << "\",\n";
        } else {
            out_ << "      \"error_code\": null,\n";
        }
        if (step.pollAttempt) {
            out_ << "      \"poll_attempt\": " << *step.pollAttempt << ",\n";
        } else {
            out_ << "      \"poll_attempt\": null,\n";
        }
        if (step.forEachIndex) {
            out_ << "      \"for_each_index\": " << *step.forEachIndex << ",\n";
        } else {
            out_ << "      \"for_each_index\": null,\n";
        }
        out_ << R"(      "detail": ")" << escape(step.detail) << "\",\n";
        out_ << "      \"assertions\": [";
        bool firstAssert = true;
        for (const auto& a : step.assertions) {
            if (!firstAssert) {
                out_ << ",";
            }
            firstAssert = false;
            out_ << "\n        { \"name\": \"" << escape(a.name) << "\", \"expr\": \""
                 << escape(a.expr) << "\", \"passed\": " << (a.passed ? "true" : "false") << " }";
        }
        if (!step.assertions.empty()) {
            out_ << "\n      ";
        }
        out_ << "]\n";
        out_ << "    }";
    }
    if (!result.steps.empty()) {
        out_ << "\n  ";
    }
    out_ << "]\n";
    out_ << "}\n";
}

}  // namespace reqloom::cli
