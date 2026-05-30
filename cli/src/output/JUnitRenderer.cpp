// JUnitRenderer — see header.
//
// JUnit XML schema matched to the surface Jenkins / GitLab / Buildkite
// consume. One <testsuite> per chain, one <testcase> per declared
// operation. Poll-attempt rows are merged into the parent <testcase>'s
// <system-out> rather than emitted as their own cases — otherwise a
// passing flow with 30 polls would inflate the suite count by 30.

#include "JUnitRenderer.h"

#include "StepFormatting.h"

#include <chainapi/engine/PublicApi.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::cli {

namespace {

namespace ce = chainapi::engine;

/// XML 1.0 attribute / text escape. Drops control characters that the
/// XML 1.0 spec disallows entirely (e.g. raw 0x00–0x08); Jenkins' parser
/// rejects whole reports if those slip through.
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
                    // Skip — XML 1.0 disallows these even when escaped.
                    break;
                }
                out += ch;
                break;
        }
    }
    return out;
}

/// Group consecutive poll-attempt rows under their parent op row. The
/// engine emits polls BEFORE the parent (see ExecutionEngine::run), so
/// we buffer poll rows forward and flush them onto the next non-poll
/// row. Trailing polls (no parent — shouldn't happen, defensive)
/// attach to the last emitted group, or are dropped if none exists.
struct GroupedStep {
    std::size_t parentIdx{};
    std::vector<std::size_t> pollIdxs;
};

std::vector<GroupedStep> groupSteps(const std::vector<ce::StepResult>& steps) {
    std::vector<GroupedStep> groups;
    std::vector<std::size_t> pendingPolls;

    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].pollAttempt) {
            pendingPolls.push_back(i);
        } else {
            groups.push_back(GroupedStep{i, std::move(pendingPolls)});
            pendingPolls.clear();
        }
    }

    if (!pendingPolls.empty() && !groups.empty()) {
        // Defensive: orphan polls past the last parent. Fold into the
        // last group so they stay visible in the report.
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
    const std::string suiteName = "chainapi." + xmlEscape(target.value);

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
         << " hostname=\"chainapi-cli\">\n";
    out_ << "    <properties>\n";
    out_ << R"(      <property name="chainapi.target" value=")" << xmlEscape(target.value)
         << "\"/>\n";
    out_ << R"(      <property name="chainapi.environment" value=")" << xmlEscape(envName)
         << "\"/>\n";
    out_ << R"(      <property name="chainapi.outcome" value=")" << runOutcomeName(result.outcome)
         << "\"/>\n";
    out_ << R"(      <property name="chainapi.run_id" value=")" << result.runId.value << "\"/>\n";
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
                out_ << "poll #" << poll.pollAttempt.value_or(0) << " " << statusGlyph(poll.status)
                     << " (" << poll.elapsed.count() << "ms)" << (poll.detail.empty() ? "" : "  ")
                     << poll.detail << "\n";
            }
            out_ << "]]></system-out>\n";
        }

        out_ << "    </testcase>\n";
    }

    out_ << "  </testsuite>\n";
    out_ << "</testsuites>\n";
}

}  // namespace chainapi::cli
