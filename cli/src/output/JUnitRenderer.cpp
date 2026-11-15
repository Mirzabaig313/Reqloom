// JUnitRenderer — see header. Poll-attempt and for_each iteration rows fold into the parent
// <testcase>, so a flow with 30 polls does not inflate the suite count.

#include "JUnitRenderer.h"

#include "StepFormatting.h"
#include "TextEncoding.h"

#include <reqloom/engine/PublicApi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;
using Status = ce::StepResult::Status;

/// Longest `message` attribute; the full text goes in the element body.
constexpr std::size_t kMessageBytes = 200;

/// A parent row and the poll / iteration rows the engine emits before it.
struct GroupedStep {
    std::size_t parentIdx{};
    std::vector<std::size_t> childIdxs;
};

std::vector<GroupedStep> groupSteps(const std::vector<ce::StepResult>& steps) {
    std::vector<GroupedStep> groups;
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].pollAttempt || steps[i].forEachIndex) {
            pending.push_back(i);
        } else {
            groups.push_back(GroupedStep{i, std::move(pending)});
            pending.clear();
        }
    }
    // Orphan rows attach to the last group so they stay visible.
    if (!groups.empty()) {
        groups.back().childIdxs.insert(
            groups.back().childIdxs.end(), pending.begin(), pending.end());
    }
    return groups;
}

/// "iter #2 poll #3", "iter #2", "poll #3", or "" for a parent row.
[[nodiscard]] std::string rowLabel(const ce::StepResult& row) {
    std::string label;
    if (row.forEachIndex) {
        label = "iter #" + std::to_string(*row.forEachIndex);
    }
    if (row.pollAttempt) {
        label +=
            (label.empty() ? "" : " ") + std::string{"poll #"} + std::to_string(*row.pollAttempt);
    }
    return label;
}

struct Problem {
    std::string type;
    std::string message;
    std::string body;
};

/// Everything one <testcase> carries, decided before any of it is written so the suite counts
/// match the cases.
struct CaseModel {
    const ce::StepResult* parent{nullptr};
    std::vector<Problem> failures;
    std::optional<Problem> error;
    bool skipped{false};
    std::string systemOut;
};

[[nodiscard]] bool failedOnlyOnAssertions(const ce::StepResult& row) {
    return row.error == ce::ErrorCode::AssertionFailed &&
           std::ranges::any_of(row.assertions, [](const auto& a) { return !a.passed; });
}

void addAssertionFailures(CaseModel& model, const ce::StepResult& row, const std::string& prefix) {
    for (const auto& a : row.assertions) {
        if (!a.passed) {
            const auto& label = a.name.empty() ? a.expr : a.name;
            model.failures.push_back(
                Problem{std::string{ce::toCodeString(ce::ErrorCode::AssertionFailed)},
                        prefix + firstLine(label, kMessageBytes),
                        a.expr});
        }
    }
}

[[nodiscard]] Problem stepFailure(const ce::StepResult& row, const std::string& prefix) {
    const auto code = row.error.value_or(ce::ErrorCode::Internal);
    const std::string_view text = row.detail.empty() ? ce::humanize(code) : row.detail;
    return Problem{
        std::string{ce::toCodeString(code)}, prefix + firstLine(text, kMessageBytes), row.detail};
}

/// One <failure> per failed assertion and per failed iteration not explained by its assertions;
/// the step's own failure only when nothing more specific explains it.
void addFailures(CaseModel& model, const std::vector<ce::StepResult>& steps, const GroupedStep& g) {
    for (const auto idx : g.childIdxs) {
        const auto& row = steps[idx];
        if (row.pollAttempt) {
            continue;
        }
        const std::string prefix = rowLabel(row) + ": ";
        addAssertionFailures(model, row, prefix);
        if (row.status == Status::Failed && !failedOnlyOnAssertions(row)) {
            model.failures.push_back(stepFailure(row, prefix));
        }
    }
    const bool iterationFailed = !model.failures.empty();
    const auto& parent = *model.parent;
    addAssertionFailures(model, parent, "");
    if (parent.status == Status::Failed &&
        (model.failures.empty() || (!iterationFailed && !failedOnlyOnAssertions(parent)))) {
        model.failures.insert(model.failures.begin(), stepFailure(parent, ""));
    }
}

void appendAssertionLines(std::string& out, const ce::StepResult& row, const std::string& label) {
    for (const auto& a : row.assertions) {
        out += label + (label.empty() ? "" : " ") + (a.passed ? "PASS" : "FAIL") +
               " assert: " + a.name + "  (" + a.expr + ")\n";
    }
}

[[nodiscard]] std::string systemOut(const std::vector<ce::StepResult>& steps,
                                    const GroupedStep& g) {
    std::string out;
    for (const auto idx : g.childIdxs) {
        const auto& row = steps[idx];
        const auto label = rowLabel(row);
        out += label + " " + std::string{statusGlyph(row.status)} + " (" +
               std::to_string(row.elapsed.count()) + "ms)" + (row.detail.empty() ? "" : "  ") +
               row.detail + "\n";
        if (!row.pollAttempt) {
            appendAssertionLines(out, row, label);
        }
    }
    appendAssertionLines(out, steps[g.parentIdx], "");
    return out;
}

[[nodiscard]] CaseModel buildCase(const std::vector<ce::StepResult>& steps, const GroupedStep& g) {
    CaseModel model;
    model.parent = &steps[g.parentIdx];
    model.systemOut = systemOut(steps, g);
    switch (model.parent->status) {
        case Status::Blocked:
            model.error = Problem{"BLOCKED", "upstream step failed; this step did not run", ""};
            break;
        case Status::Cancelled:
            model.error = Problem{"CANCELLED", "run was cancelled", ""};
            break;
        case Status::Skipped:
            model.skipped = true;
            break;
        default:
            addFailures(model, steps, g);
            break;
    }
    return model;
}

struct Counts {
    std::size_t tests{0};
    std::size_t failures{0};
    std::size_t errors{0};
    std::size_t skipped{0};
};

[[nodiscard]] Counts countCases(const std::vector<CaseModel>& cases) {
    Counts c;
    c.tests = cases.size();
    for (const auto& model : cases) {
        if (model.error) {
            ++c.errors;
        } else if (!model.failures.empty()) {
            ++c.failures;
        } else if (model.skipped) {
            ++c.skipped;
        }
    }
    return c;
}

void writeProblem(std::ostream& out, std::string_view element, const Problem& p) {
    out << "      <" << element << " type=\"" << xmlAttribute(p.type) << "\" message=\""
        << xmlAttribute(p.message) << "\"";
    if (p.body.empty()) {
        out << "/>\n";
    } else {
        out << ">" << xmlText(p.body) << "</" << element << ">\n";
    }
}

struct SuiteHead {
    std::string_view target;
    std::string_view environment;
    std::string_view outcome;
    std::optional<ce::RunId> run;
    Counts counts;
    double seconds{0.0};
};

void writeCounts(std::ostream& out, const SuiteHead& h) {
    out << " tests=\"" << h.counts.tests << "\" failures=\"" << h.counts.failures << "\" errors=\""
        << h.counts.errors << "\" skipped=\"" << h.counts.skipped << "\" time=\"" << h.seconds
        << "\"";
}

void writeHead(std::ostream& out, const SuiteHead& h) {
    const std::string name = xmlAttribute(h.target);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<testsuites name=\"" << name << "\"";
    writeCounts(out, h);
    out << ">\n";
    out << "  <testsuite name=\"reqloom." << name << "\"";
    writeCounts(out, h);
    out << " hostname=\"reqloom-cli\">\n";
    out << "    <properties>\n";
    out << R"(      <property name="reqloom.target" value=")" << name << "\"/>\n";
    out << R"(      <property name="reqloom.environment" value=")" << xmlAttribute(h.environment)
        << "\"/>\n";
    out << R"(      <property name="reqloom.outcome" value=")" << h.outcome << "\"/>\n";
    if (h.run) {
        out << R"(      <property name="reqloom.run_id" value=")" << h.run->value << "\"/>\n";
    }
    out << "    </properties>\n";
}

void writeTail(std::ostream& out) {
    out << "  </testsuite>\n";
    out << "</testsuites>\n";
}

void writeCase(std::ostream& out, std::string_view classname, const CaseModel& model) {
    const auto& parent = *model.parent;
    const double seconds = static_cast<double>(parent.elapsed.count()) / 1000.0;
    out << "    <testcase classname=\"reqloom." << xmlAttribute(classname) << "\" name=\""
        << xmlAttribute(parent.op.value) << "\" time=\"" << seconds << "\">\n";
    if (model.error) {
        writeProblem(out, "error", *model.error);
    }
    for (const auto& failure : model.failures) {
        writeProblem(out, "failure", failure);
    }
    if (model.skipped) {
        out << "      <skipped message=\"step skipped (cached extraction)\"/>\n";
    }
    if (!model.systemOut.empty()) {
        out << "      <system-out>" << xmlCdata(model.systemOut) << "</system-out>\n";
    }
    out << "    </testcase>\n";
}

}  // namespace

JUnitRenderer::JUnitRenderer(std::ostream& out) : out_(out) {}

void JUnitRenderer::render(const ce::OperationId& target,
                           std::string_view environment,
                           const ce::RunResult& result) {
    std::vector<CaseModel> cases;
    std::int64_t totalMs = 0;
    for (const auto& g : groupSteps(result.steps)) {
        cases.push_back(buildCase(result.steps, g));
        totalMs += result.steps[g.parentIdx].elapsed.count();
    }
    writeHead(out_,
              SuiteHead{target.value,
                        environment,
                        runOutcomeName(result.outcome),
                        result.runId,
                        countCases(cases),
                        static_cast<double>(totalMs) / 1000.0});
    for (const auto& model : cases) {
        writeCase(out_, target.value, model);
    }
    writeTail(out_);
}

void JUnitRenderer::renderError(const ce::OperationId& target,
                                std::string_view environment,
                                std::optional<ce::RunId> run,
                                std::string_view code,
                                std::string_view detail) {
    const std::string_view name = target.value.empty() ? std::string_view{"reqloom"} : target.value;
    writeHead(out_, SuiteHead{name, environment, kErrorOutcomeName, run, Counts{1, 0, 1, 0}, 0.0});
    out_ << "    <testcase classname=\"reqloom." << xmlAttribute(name) << "\" name=\""
         << xmlAttribute(name) << "\" time=\"0\">\n";
    writeProblem(out_,
                 "error",
                 Problem{std::string{code}, firstLine(detail, kMessageBytes), std::string{detail}});
    out_ << "    </testcase>\n";
    writeTail(out_);
}

}  // namespace reqloom::cli
