// JUnitRenderer — see header. Poll-attempt rows fold into the parent <testcase>'s <system-out>,
// so a flow with 30 polls does not inflate the suite count.

#include "JUnitRenderer.h"

#include "StepFormatting.h"

#include <reqloom/engine/PublicApi.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;

/// XML 1.0 escape; drops control characters the spec disallows (Jenkins rejects whole reports
/// containing them).
std::string xmlEscape(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (ch) {
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '&':
                out += "&amp;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                if (byte < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
                    break;
                }
                out += ch;
                break;
        }
    }
    return out;
}

/// Group poll-attempt rows under their parent op row. The engine emits polls before the parent,
/// so poll rows buffer until the next non-poll row.
struct GroupedStep {
    std::size_t parentIdx{};
    std::vector<std::size_t> pollIdxs;
};

std::vector<GroupedStep> groupSteps(const std::vector<ce::StepResult>& steps) {
    std::vector<GroupedStep> groups;
    std::vector<std::size_t> pendingPolls;

    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].pollAttempt || steps[i].forEachIndex) {
            pendingPolls.push_back(i);
        } else {
            groups.push_back(GroupedStep{i, std::move(pendingPolls)});
            pendingPolls.clear();
        }
    }

    if (!pendingPolls.empty() && !groups.empty()) {
        // Orphan polls attach to the last group so they stay visible.
        for (auto idx : pendingPolls) {
            groups.back().pollIdxs.push_back(idx);
        }
    }
    return groups;
}

}  // namespace

JUnitRenderer::JUnitRenderer(std::ostream& out) : out_(out) {}

void JUnitRenderer::render(const ce::OperationId& target,
                           std::string_view environment,
                           const ce::RunResult& result) {
    const auto groups = groupSteps(result.steps);

    std::uint32_t failures = 0;
    std::uint32_t errors = 0;
    std::uint32_t skipped = 0;
    std::int64_t totalMs = 0;
    for (const auto& g : groups) {
        const auto& parent = result.steps[g.parentIdx];
        totalMs += parent.elapsed.count();
        switch (parent.status) {
            case ce::StepResult::Status::Failed:
                ++failures;
                break;
            case ce::StepResult::Status::Cancelled:
            case ce::StepResult::Status::Blocked:
                ++errors;
                break;
            case ce::StepResult::Status::Skipped:
                ++skipped;
                break;
            default:
                break;
        }
    }

    const double seconds = static_cast<double>(totalMs) / 1000.0;
    const std::string envName{environment.empty() ? std::string_view{"default"} : environment};
    const std::string suiteName = "reqloom." + xmlEscape(target.value);

    out_ << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out_ << "<testsuites name=\"" << xmlEscape(target.value) << "\" tests=\"" << groups.size()
         << "\""
         << " failures=\"" << failures << "\""
         << " errors=\"" << errors << "\""
         << " skipped=\"" << skipped << "\""
         << " time=\"" << seconds << "\">\n";
    out_ << "  <testsuite name=\"" << suiteName << "\" tests=\"" << groups.size() << "\""
         << " failures=\"" << failures << "\""
         << " errors=\"" << errors << "\""
         << " skipped=\"" << skipped << "\""
         << " time=\"" << seconds << "\""
         << " hostname=\"reqloom-cli\">\n";
    out_ << "    <properties>\n";
    out_ << R"(      <property name="reqloom.target" value=")" << xmlEscape(target.value)
         << "\"/>\n";
    out_ << R"(      <property name="reqloom.environment" value=")" << xmlEscape(envName)
         << "\"/>\n";
    out_ << R"(      <property name="reqloom.outcome" value=")" << runOutcomeName(result.outcome)
         << "\"/>\n";
    out_ << R"(      <property name="reqloom.run_id" value=")" << result.runId.value << "\"/>\n";
    out_ << "    </properties>\n";

    for (const auto& g : groups) {
        const auto& parent = result.steps[g.parentIdx];
        const double caseSeconds = static_cast<double>(parent.elapsed.count()) / 1000.0;
        out_ << "    <testcase classname=\"" << suiteName << "\" name=\""
             << xmlEscape(parent.op.value) << "\" time=\"" << caseSeconds << "\">\n";

        switch (parent.status) {
            case ce::StepResult::Status::Failed: {
                const std::string code =
                    parent.error ? std::string(ce::toCodeString(*parent.error)) : std::string{"E"};
                out_ << "      <failure type=\"" << xmlEscape(code) << "\""
                     << " message=\"" << xmlEscape(parent.detail) << "\"/>\n";
                break;
            }
            case ce::StepResult::Status::Skipped:
                out_ << "      <skipped message=\"step skipped (cached extraction)\"/>\n";
                break;
            case ce::StepResult::Status::Blocked:
                out_ << "      <error type=\"BLOCKED\""
                     << " message=\"upstream step failed; this step did not run\"/>\n";
                break;
            case ce::StepResult::Status::Cancelled:
                out_ << "      <error type=\"CANCELLED\""
                     << " message=\"run was cancelled\"/>\n";
                break;
            default:
                break;
        }

        if (!g.pollIdxs.empty()) {
            out_ << "      <system-out><![CDATA[";
            for (auto idx : g.pollIdxs) {
                const auto& poll = result.steps[idx];
                const std::string tag =
                    poll.forEachIndex ? "iter #" + std::to_string(*poll.forEachIndex)
                                      : "poll #" + std::to_string(poll.pollAttempt.value_or(0));
                out_ << tag << " " << statusGlyph(poll.status) << " (" << poll.elapsed.count()
                     << "ms)" << (poll.detail.empty() ? "" : "  ") << poll.detail << "\n";
            }
            out_ << "]]></system-out>\n";
        }

        if (!parent.assertions.empty()) {
            out_ << "      <system-out><![CDATA[";
            for (const auto& a : parent.assertions) {
                out_ << (a.passed ? "PASS" : "FAIL") << " assert: " << a.name << "  (" << a.expr
                     << ")\n";
            }
            out_ << "]]></system-out>\n";
        }

        out_ << "    </testcase>\n";
    }

    out_ << "  </testsuite>\n";
    out_ << "</testsuites>\n";
}

}  // namespace reqloom::cli
