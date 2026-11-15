// Renderer unit tests — exercise the three CLI output formats against
// hand-crafted RunResult fixtures. Each test asserts on substrings
// rather than full-document equality so the schema can evolve without
// rewriting every fixture, but the contract bits CI consumers depend on
// (status counts, error codes, JUnit attributes) are pinned.

#include "output/JUnitRenderer.h"
#include "output/JsonRenderer.h"
#include "output/TextRenderer.h"

#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QXmlStreamReader>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;
namespace cli = reqloom::cli;

namespace {

ce::RunResult buildSuccessfulRun() {
    ce::RunResult r;
    r.runId = ce::RunId{42};
    r.outcome = ce::RunOutcome::Succeeded;

    ce::StepResult login;
    login.op = ce::OperationId{"actor.login"};
    login.status = ce::StepResult::Status::Succeeded;
    login.elapsed = std::chrono::milliseconds{17};
    login.attempts = 1;
    r.steps.push_back(login);

    ce::StepResult target;
    target.op = ce::OperationId{"order.create"};
    target.status = ce::StepResult::Status::Succeeded;
    target.elapsed = std::chrono::milliseconds{83};
    target.attempts = 1;
    r.steps.push_back(target);
    return r;
}

ce::RunResult buildFailedRun() {
    ce::RunResult r;
    r.runId = ce::RunId{7};
    r.outcome = ce::RunOutcome::Failed;

    ce::StepResult login;
    login.op = ce::OperationId{"actor.login"};
    login.status = ce::StepResult::Status::Succeeded;
    login.elapsed = std::chrono::milliseconds{12};
    login.attempts = 1;
    r.steps.push_back(login);

    ce::StepResult failed;
    failed.op = ce::OperationId{"order.pay"};
    failed.status = ce::StepResult::Status::Failed;
    failed.error = ce::ErrorCode::Http4xx;
    failed.elapsed = std::chrono::milliseconds{45};
    failed.attempts = 2;
    failed.detail = "HTTP 422 — validation: \"email\" is required";
    r.steps.push_back(failed);

    ce::StepResult blocked;
    blocked.op = ce::OperationId{"order.confirm"};
    blocked.status = ce::StepResult::Status::Blocked;
    r.steps.push_back(blocked);
    return r;
}

ce::RunResult buildRunWithPolling() {
    ce::RunResult r;
    r.runId = ce::RunId{9};
    r.outcome = ce::RunOutcome::Succeeded;

    // Engine emits poll-attempt rows BEFORE the parent op row.
    ce::StepResult poll1;
    poll1.op = ce::OperationId{"payment.charge"};
    poll1.pollAttempt = 1;
    poll1.status = ce::StepResult::Status::Pending;
    poll1.detail = "in_progress (HTTP 202)";
    poll1.elapsed = std::chrono::milliseconds{30};
    r.steps.push_back(poll1);

    ce::StepResult poll2;
    poll2.op = ce::OperationId{"payment.charge"};
    poll2.pollAttempt = 2;
    poll2.status = ce::StepResult::Status::Succeeded;
    poll2.detail = "success_when matched (HTTP 200)";
    poll2.elapsed = std::chrono::milliseconds{40};
    r.steps.push_back(poll2);

    ce::StepResult parent;
    parent.op = ce::OperationId{"payment.charge"};
    parent.status = ce::StepResult::Status::Succeeded;
    parent.elapsed = std::chrono::milliseconds{120};
    parent.attempts = 1;
    r.steps.push_back(parent);
    return r;
}

}  // namespace

// ─── TextRenderer ────────────────────────────────────────────────────────────

TEST(TextRenderer, prints_summary_with_op_and_status) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};

    renderer.render(ce::OperationId{"order.create"}, "local", buildSuccessfulRun());

    const auto out = summary.str();
    EXPECT_NE(out.find("Chain Summary"), std::string::npos);
    EXPECT_NE(out.find("Target: order.create"), std::string::npos);
    EXPECT_NE(out.find("Outcome: SUCCEEDED"), std::string::npos);
    EXPECT_NE(out.find("OK"), std::string::npos);
    EXPECT_NE(out.find("actor.login"), std::string::npos);
    EXPECT_NE(out.find("order.create"), std::string::npos);
}

TEST(TextRenderer, includes_failure_detail_in_summary) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};

    renderer.render(ce::OperationId{"order.pay"}, "staging", buildFailedRun());

    const auto out = summary.str();
    EXPECT_NE(out.find("Outcome: FAILED"), std::string::npos);
    EXPECT_NE(out.find("FAIL"), std::string::npos);
    EXPECT_NE(out.find("E_HTTP_4XX"), std::string::npos);
    EXPECT_NE(out.find("HTTP 422"), std::string::npos);
    EXPECT_NE(out.find("BLOCK"), std::string::npos);
}

TEST(TextRenderer, quiet_suppresses_progress_but_keeps_failure_on_stderr) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};

    ce::StepFailed failedEvent;
    failedEvent.runId = ce::RunId{1};
    failedEvent.stepIndex = 0;
    failedEvent.op = ce::OperationId{"order.pay"};
    failedEvent.code = ce::ErrorCode::Http4xx;
    failedEvent.cls = ce::ErrorClass::Http;
    failedEvent.attempt = 1;
    failedEvent.detail = "HTTP 422";
    renderer.onEvent(failedEvent);

    EXPECT_TRUE(progress.str().empty());
    EXPECT_NE(err.str().find("FAILED: order.pay"), std::string::npos);
    EXPECT_NE(err.str().find("E_HTTP_4XX"), std::string::npos);
}

TEST(TextRenderer, renders_poll_attempts_under_parent_op) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};

    renderer.render(ce::OperationId{"payment.charge"}, "local", buildRunWithPolling());

    const auto out = summary.str();
    EXPECT_NE(out.find("poll #1"), std::string::npos);
    EXPECT_NE(out.find("poll #2"), std::string::npos);
    EXPECT_NE(out.find("success_when matched (HTTP 200)"), std::string::npos);
}

// ─── JsonRenderer ────────────────────────────────────────────────────────────

TEST(JsonRenderer, emits_well_formed_object_with_required_fields) {
    std::ostringstream out;
    cli::JsonRenderer renderer{out};

    renderer.render(ce::OperationId{"order.create"}, "local", buildSuccessfulRun());

    const auto json = out.str();
    EXPECT_NE(json.find("\"run_id\": 42"), std::string::npos);
    EXPECT_NE(json.find("\"target\": \"order.create\""), std::string::npos);
    EXPECT_NE(json.find("\"environment\": \"local\""), std::string::npos);
    EXPECT_NE(json.find("\"outcome\": \"SUCCEEDED\""), std::string::npos);
    EXPECT_NE(json.find("\"succeeded\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"failed\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"op\": \"actor.login\""), std::string::npos);
    EXPECT_NE(json.find("\"status\": \"OK\""), std::string::npos);
}

TEST(JsonRenderer, escapes_quotes_and_control_chars_in_detail) {
    std::ostringstream out;
    cli::JsonRenderer renderer{out};

    renderer.render(ce::OperationId{"order.pay"}, "staging", buildFailedRun());

    const auto json = out.str();
    EXPECT_NE(json.find("\"outcome\": \"FAILED\""), std::string::npos);
    EXPECT_NE(json.find("\"failed\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"blocked\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"error_code\": \"E_HTTP_4XX\""), std::string::npos);
    // The detail contains a literal `"` which must be escaped as `\"`.
    EXPECT_NE(json.find("\\\"email\\\""), std::string::npos);
}

TEST(JsonRenderer, counts_poll_attempts_separately_from_step_totals) {
    std::ostringstream out;
    cli::JsonRenderer renderer{out};

    renderer.render(ce::OperationId{"payment.charge"}, "local", buildRunWithPolling());

    const auto json = out.str();
    EXPECT_NE(json.find("\"succeeded\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"poll_attempts\": 2"), std::string::npos);
}

// ─── JUnitRenderer ───────────────────────────────────────────────────────────

TEST(JUnitRenderer, emits_one_testcase_per_op_with_correct_attributes) {
    std::ostringstream out;
    cli::JUnitRenderer renderer{out};

    renderer.render(ce::OperationId{"order.create"}, "local", buildSuccessfulRun());

    const auto xml = out.str();
    EXPECT_NE(xml.find("<?xml version=\"1.0\""), std::string::npos);
    EXPECT_NE(xml.find("<testsuites"), std::string::npos);
    EXPECT_NE(xml.find("tests=\"2\""), std::string::npos);
    EXPECT_NE(xml.find("failures=\"0\""), std::string::npos);
    EXPECT_NE(xml.find("name=\"actor.login\""), std::string::npos);
    EXPECT_NE(xml.find("name=\"order.create\""), std::string::npos);
    EXPECT_NE(xml.find("reqloom.target"), std::string::npos);
}

TEST(JUnitRenderer, marks_failed_step_as_failure_and_blocked_step_as_error) {
    std::ostringstream out;
    cli::JUnitRenderer renderer{out};

    renderer.render(ce::OperationId{"order.pay"}, "staging", buildFailedRun());

    const auto xml = out.str();
    EXPECT_NE(xml.find("failures=\"1\""), std::string::npos);
    EXPECT_NE(xml.find("errors=\"1\""), std::string::npos);
    EXPECT_NE(xml.find("<failure"), std::string::npos);
    EXPECT_NE(xml.find("type=\"E_HTTP_4XX\""), std::string::npos);
    EXPECT_NE(xml.find("<error"), std::string::npos);
    EXPECT_NE(xml.find("type=\"BLOCKED\""), std::string::npos);
    // The detail string contains a `"` and is going into an attribute.
    EXPECT_NE(xml.find("&quot;email&quot;"), std::string::npos);
}

TEST(JUnitRenderer, folds_poll_attempts_into_parent_testcase_systemout) {
    std::ostringstream out;
    cli::JUnitRenderer renderer{out};

    renderer.render(ce::OperationId{"payment.charge"}, "local", buildRunWithPolling());

    const auto xml = out.str();
    // One declared op → one testcase, regardless of how many polls.
    EXPECT_NE(xml.find("tests=\"1\""), std::string::npos);
    EXPECT_NE(xml.find("<system-out><![CDATA["), std::string::npos);
    EXPECT_NE(xml.find("poll #1"), std::string::npos);
    EXPECT_NE(xml.find("poll #2"), std::string::npos);
}

// ─── Captured bodies ─────────────────────────────────────────────────────────

// Captured bodies are unmasked for the desktop. CLI output can be read by an AI
// agent, so a body may only appear there through `ce::maskCapturedBody`.
TEST(CliOutput, no_captured_body_text_appears_in_any_format) {
    const std::string body =
        R"({"access_token":"CAPTURED-BODY-TOKEN","note":"CAPTURED-BODY-TEXT"})";
    const std::vector<ce::RunEvent> events{
        ce::RunStarted{ce::RunId{7}, ce::OperationId{"order.pay"}, 2, "staging", {}},
        ce::StepStarted{ce::RunId{7}, 0, ce::OperationId{"actor.login"}, 1, {}},
        ce::ResponseReceived{ce::RunId{7}, 0, 200, {}, body.size(), {}, {}, body},
        ce::ResponseReceived{ce::RunId{7}, 1, 422, {}, body.size(), {}, {}, body},
        ce::StepFailed{ce::RunId{7},
                       1,
                       ce::OperationId{"order.pay"},
                       ce::ErrorCode::Http4xx,
                       ce::ErrorClass::Http,
                       1,
                       "HTTP 422 (expected 201)",
                       {},
                       {}},
        ce::RunEnded{ce::RunId{7}, ce::RunOutcome::Failed, {}, {}},
    };
    const auto run = buildFailedRun();

    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer text{summary, progress, err, /*quiet=*/false};
    for (const auto& event : events) {
        text.onEvent(event);
    }
    text.render(ce::OperationId{"order.pay"}, "staging", run);

    std::ostringstream json;
    cli::JsonRenderer{json}.render(ce::OperationId{"order.pay"}, "staging", run);
    std::ostringstream junit;
    cli::JUnitRenderer{junit}.render(ce::OperationId{"order.pay"}, "staging", run);

    for (const auto& out : {summary.str() + progress.str() + err.str(), json.str(), junit.str()}) {
        EXPECT_EQ(out.find("CAPTURED-BODY"), std::string::npos) << out;
    }
    EXPECT_NE(err.str().find("HTTP 422"), std::string::npos) << "the failure itself is printed";
}

// ─── Well-formedness, attribution and counts ─────────────────────────────────

namespace {

[[nodiscard]] bool wellFormedXml(const std::string& text) {
    QXmlStreamReader reader(QByteArray::fromStdString(text));
    while (!reader.atEnd()) {
        reader.readNext();
    }
    return !reader.hasError();
}

struct XmlElement {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
};

/// Every element with its attributes and the text directly inside it, in document order.
[[nodiscard]] std::vector<XmlElement> xmlElements(const std::string& text) {
    std::vector<XmlElement> elements;
    std::vector<std::size_t> open;
    QXmlStreamReader reader(QByteArray::fromStdString(text));
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            XmlElement element;
            element.name = reader.name().toString().toStdString();
            for (const auto& attribute : reader.attributes()) {
                element.attributes[attribute.name().toString().toStdString()] =
                    attribute.value().toString().toStdString();
            }
            open.push_back(elements.size());
            elements.push_back(std::move(element));
        } else if (reader.isEndElement() && !open.empty()) {
            open.pop_back();
        } else if (reader.isCharacters() && !open.empty()) {
            elements[open.back()].text += reader.text().toString().toStdString();
        }
    }
    return elements;
}

[[nodiscard]] std::size_t countNamed(const std::vector<XmlElement>& elements,
                                     std::string_view name) {
    return static_cast<std::size_t>(
        std::ranges::count_if(elements, [&](const XmlElement& e) { return e.name == name; }));
}

[[nodiscard]] const XmlElement& firstNamed(const std::vector<XmlElement>& elements,
                                           std::string_view name) {
    return *std::ranges::find_if(elements, [&](const XmlElement& e) { return e.name == name; });
}

[[nodiscard]] ce::StepResult row(std::string op, ce::StepResult::Status status) {
    ce::StepResult step;
    step.op = ce::OperationId{std::move(op)};
    step.status = status;
    step.attempts = 1;
    return step;
}

/// A for_each step over two items: iteration 1 polls twice and passes, iteration 2 fails an
/// assertion. The engine records rows iteration by iteration, then the parent.
ce::RunResult buildForEachRun() {
    ce::RunResult r;
    r.runId = ce::RunId{11};
    r.outcome = ce::RunOutcome::Failed;
    using S = ce::StepResult::Status;

    auto poll1 = row("org.detail", S::Pending);
    poll1.forEachIndex = 1;
    poll1.pollAttempt = 1;
    poll1.detail = "in_progress (HTTP 202)";
    r.steps.push_back(poll1);
    auto poll2 = row("org.detail", S::Succeeded);
    poll2.forEachIndex = 1;
    poll2.pollAttempt = 2;
    poll2.detail = "success_when matched (HTTP 200)";
    r.steps.push_back(poll2);
    auto iter1 = row("org.detail", S::Succeeded);
    iter1.forEachIndex = 1;
    iter1.assertions.push_back({"has name", "$.name != null", true});
    r.steps.push_back(iter1);
    auto iter2 = row("org.detail", S::Failed);
    iter2.forEachIndex = 2;
    iter2.error = ce::ErrorCode::AssertionFailed;
    iter2.detail = "assertion failed: has name";
    iter2.assertions.push_back({"has name", "$.name != null", false});
    r.steps.push_back(iter2);
    auto parent = row("org.detail", S::Failed);
    parent.error = ce::ErrorCode::AssertionFailed;
    parent.detail = "assertion failed: has name";
    r.steps.push_back(parent);
    return r;
}

/// One step that failed two of three assertions, with a multi-line detail.
ce::RunResult buildAssertionRun(ce::ErrorCode code) {
    ce::RunResult r;
    r.runId = ce::RunId{12};
    r.outcome = ce::RunOutcome::Failed;
    auto step = row("user.get", ce::StepResult::Status::Failed);
    step.error = code;
    step.detail = "first line\nsecond line";
    step.assertions.push_back({"status ok", "$.status_code == 200", true});
    step.assertions.push_back({"has id", "$.id != null", false});
    step.assertions.push_back({"", "$.name == \"x\"", false});
    r.steps.push_back(step);
    return r;
}

}  // namespace

TEST(JUnitRenderer, cdata_terminator_and_forbidden_characters_stay_well_formed) {
    auto run = buildRunWithPolling();
    run.steps[0].detail = "body ]]> after \x01\x1b bell \xEF\xBF\xBF lone \xED\xA0\x80 bad \xC3";
    run.steps[2].op = ce::OperationId{
        "payment.\x02"
        "charge"};
    run.steps[2].assertions.push_back({"a ]]> b", "x ]]> y", true});

    std::ostringstream out;
    cli::JUnitRenderer{out}.render(ce::OperationId{"payment.charge"}, "local", run);

    ASSERT_TRUE(wellFormedXml(out.str())) << out.str();
    const auto systemOut = firstNamed(xmlElements(out.str()), "system-out").text;
    EXPECT_NE(systemOut.find("body ]]> after"), std::string::npos) << systemOut;
    EXPECT_NE(systemOut.find("a ]]> b"), std::string::npos) << systemOut;
}

TEST(JUnitRenderer, one_system_out_per_testcase_with_polls_and_assertions) {
    auto run = buildRunWithPolling();
    run.steps[2].assertions.push_back({"settled", "$.status == \"COMPLETED\"", true});

    std::ostringstream out;
    cli::JUnitRenderer{out}.render(ce::OperationId{"payment.charge"}, "local", run);

    EXPECT_EQ(countNamed(xmlElements(out.str()), "system-out"), 1u) << out.str();
}

TEST(JUnitRenderer, one_failure_per_failed_assertion_without_a_duplicate_step_failure) {
    std::ostringstream out;
    cli::JUnitRenderer{out}.render(
        ce::OperationId{"user.get"}, "local", buildAssertionRun(ce::ErrorCode::AssertionFailed));

    const auto elements = xmlElements(out.str());
    ASSERT_EQ(countNamed(elements, "failure"), 2u) << out.str();
    std::vector<std::string> messages;
    for (const auto& e : elements) {
        if (e.name == "failure") {
            messages.push_back(e.attributes.at("message"));
        }
    }
    EXPECT_EQ(messages[0], "has id");
    EXPECT_EQ(messages[1], "$.name == \"x\"");
    EXPECT_NE(out.str().find("failures=\"1\""), std::string::npos) << out.str();
}

TEST(JUnitRenderer, step_failure_detail_goes_in_the_body_with_a_single_line_message) {
    std::ostringstream out;
    cli::JUnitRenderer{out}.render(
        ce::OperationId{"user.get"}, "local", buildAssertionRun(ce::ErrorCode::Http4xx));

    const auto elements = xmlElements(out.str());
    ASSERT_EQ(countNamed(elements, "failure"), 3u) << out.str();
    const auto& stepFailure = firstNamed(elements, "failure");
    EXPECT_EQ(stepFailure.attributes.at("type"), "E_HTTP_4XX");
    EXPECT_EQ(stepFailure.attributes.at("message").find('\n'), std::string::npos);
    EXPECT_NE(stepFailure.text.find("first line\nsecond line"), std::string::npos)
        << stepFailure.text;
}

TEST(JUnitRenderer, for_each_iteration_assertions_and_polls_are_attributed) {
    std::ostringstream out;
    cli::JUnitRenderer{out}.render(ce::OperationId{"org.detail"}, "local", buildForEachRun());

    ASSERT_TRUE(wellFormedXml(out.str())) << out.str();
    const auto elements = xmlElements(out.str());
    EXPECT_EQ(countNamed(elements, "testcase"), 1u);
    ASSERT_EQ(countNamed(elements, "failure"), 1u) << out.str();
    EXPECT_NE(firstNamed(elements, "failure").attributes.at("message").find("iter #2"),
              std::string::npos)
        << out.str();
    EXPECT_NE(out.str().find("iter #1 poll #2"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("iter #1 PASS assert: has name"), std::string::npos) << out.str();
}

TEST(JUnitRenderer, counts_match_the_emitted_cases) {
    for (const auto& run : {buildSuccessfulRun(),
                            buildFailedRun(),
                            buildRunWithPolling(),
                            buildForEachRun(),
                            buildAssertionRun(ce::ErrorCode::Http4xx)}) {
        std::ostringstream out;
        cli::JUnitRenderer{out}.render(ce::OperationId{"t"}, "local", run);
        const auto elements = xmlElements(out.str());
        std::size_t cases = 0;
        std::set<std::size_t> failed;
        std::set<std::size_t> errored;
        std::set<std::size_t> skipped;
        for (const auto& e : elements) {
            if (e.name == "testcase") {
                ++cases;
            } else if (e.name == "failure") {
                failed.insert(cases);
            } else if (e.name == "error") {
                errored.insert(cases);
            } else if (e.name == "skipped") {
                skipped.insert(cases);
            }
        }
        for (const auto* name : {"testsuites", "testsuite"}) {
            const auto& suite = firstNamed(elements, name);
            EXPECT_EQ(suite.attributes.at("tests"), std::to_string(cases)) << out.str();
            EXPECT_EQ(suite.attributes.at("failures"), std::to_string(failed.size())) << out.str();
            EXPECT_EQ(suite.attributes.at("errors"), std::to_string(errored.size())) << out.str();
            EXPECT_EQ(suite.attributes.at("skipped"), std::to_string(skipped.size())) << out.str();
        }
    }
}

TEST(JUnitRenderer, error_document_has_one_errored_case) {
    std::ostringstream out;
    cli::JUnitRenderer{out}.renderError(ce::OperationId{"user.get"},
                                        "local",
                                        std::nullopt,
                                        "E_CLI_UNKNOWN_OPERATION",
                                        "unknown operation 'user.get'\nsecond <line>");
    ASSERT_TRUE(wellFormedXml(out.str())) << out.str();
    const auto elements = xmlElements(out.str());
    EXPECT_EQ(countNamed(elements, "testcase"), 1u);
    ASSERT_EQ(countNamed(elements, "error"), 1u);
    const auto& error = firstNamed(elements, "error");
    EXPECT_EQ(error.attributes.at("type"), "E_CLI_UNKNOWN_OPERATION");
    EXPECT_EQ(error.attributes.at("message"), "unknown operation 'user.get'");
    EXPECT_NE(error.text.find("second <line>"), std::string::npos);
    EXPECT_NE(out.str().find("errors=\"1\""), std::string::npos);
}

TEST(TextRenderer, for_each_iteration_assertions_and_polls_are_attributed) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};
    renderer.render(ce::OperationId{"org.detail"}, "local", buildForEachRun());

    const auto out = summary.str();
    EXPECT_NE(out.find("iter #1 poll #2"), std::string::npos) << out;
    EXPECT_NE(out.find("assert (iter #2): has name"), std::string::npos) << out;
}

TEST(JsonRenderer, document_has_schema_version_and_null_error) {
    std::ostringstream out;
    cli::JsonRenderer{out}.render(ce::OperationId{"order.create"}, "local", buildSuccessfulRun());
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(out.str())).object();
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1);
    EXPECT_TRUE(doc.value(QStringLiteral("error")).isNull());
    EXPECT_EQ(doc.value(QStringLiteral("run_id")).toInt(), 42);
}

TEST(JsonRenderer, invalid_utf8_is_replaced_so_the_document_stays_valid) {
    auto run = buildFailedRun();
    run.steps[1].op = ce::OperationId{"order.\xFFpay"};
    run.steps[1].detail = "bad \xC3 then \xED\xA0\x80 then ok \xC3\xA9";
    std::ostringstream out;
    cli::JsonRenderer{out}.render(ce::OperationId{"order.\xC0\xAFpay"}, "staging", run);

    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(out.str()), &error);
    ASSERT_EQ(error.error, QJsonParseError::NoError) << out.str();
    const auto step = doc.object().value(QStringLiteral("steps")).toArray().at(1).toObject();
    EXPECT_EQ(step.value(QStringLiteral("op")).toString(),
              QString::fromUtf8("order.\xEF\xBF\xBDpay"));
    EXPECT_TRUE(
        step.value(QStringLiteral("detail")).toString().endsWith(QString::fromUtf8("ok \xC3\xA9")));
}

TEST(JsonRenderer, error_document_keeps_the_run_shape) {
    std::ostringstream out;
    cli::JsonRenderer{out}.renderError(ce::OperationId{"user.get"},
                                       "local",
                                       ce::RunId{5},
                                       "E_SECRET_ACCESS_FAILED",
                                       "no keychain backend");
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(out.str())).object();
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1);
    EXPECT_EQ(doc.value(QStringLiteral("run_id")).toInt(), 5);
    EXPECT_EQ(doc.value(QStringLiteral("target")).toString(), QStringLiteral("user.get"));
    EXPECT_EQ(doc.value(QStringLiteral("environment")).toString(), QStringLiteral("local"));
    EXPECT_EQ(doc.value(QStringLiteral("outcome")).toString(), QStringLiteral("ERROR"));
    EXPECT_TRUE(doc.value(QStringLiteral("summary")).isObject());
    EXPECT_TRUE(doc.value(QStringLiteral("steps")).toArray().isEmpty());
    const auto error = doc.value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toString(),
              QStringLiteral("E_SECRET_ACCESS_FAILED"));
    EXPECT_EQ(error.value(QStringLiteral("detail")).toString(),
              QStringLiteral("no keychain backend"));
}

namespace {

ce::RunResult buildExplainedAssertionRun() {
    auto run = buildAssertionRun(ce::ErrorCode::AssertionFailed);
    run.steps[0].assertions[1].detail = "$.id was null, expected != null";
    return run;
}

}  // namespace

TEST(JUnitRenderer, an_assertion_failure_body_says_why_then_gives_the_expression) {
    std::ostringstream out;
    cli::JUnitRenderer{out}.render(
        ce::OperationId{"user.get"}, "local", buildExplainedAssertionRun());

    const auto elements = xmlElements(out.str());
    const auto& failure = firstNamed(elements, "failure");
    EXPECT_EQ(failure.attributes.at("message"), "has id");
    EXPECT_EQ(failure.text, "$.id was null, expected != null\n$.id != null") << out.str();
}

TEST(JsonRenderer, each_assertion_carries_its_detail) {
    std::ostringstream out;
    cli::JsonRenderer{out}.render(
        ce::OperationId{"user.get"}, "local", buildExplainedAssertionRun());
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(out.str())).object();
    const auto assertions = doc.value(QStringLiteral("steps"))
                                .toArray()
                                .at(0)
                                .toObject()
                                .value(QStringLiteral("assertions"))
                                .toArray();
    ASSERT_EQ(assertions.size(), 3);
    EXPECT_EQ(assertions.at(0).toObject().value(QStringLiteral("detail")).toString(), QString{});
    EXPECT_EQ(assertions.at(1).toObject().value(QStringLiteral("detail")).toString(),
              QStringLiteral("$.id was null, expected != null"));
    EXPECT_EQ(assertions.at(1).toObject().value(QStringLiteral("expr")).toString(),
              QStringLiteral("$.id != null"));
}

TEST(TextRenderer, a_failed_assertion_is_followed_by_why) {
    std::ostringstream summary;
    std::ostringstream progress;
    std::ostringstream err;
    cli::TextRenderer renderer{summary, progress, err, /*quiet=*/true};
    renderer.render(ce::OperationId{"user.get"}, "local", buildExplainedAssertionRun());

    const auto out = summary.str();
    const auto assertLine = out.find("assert: has id");
    ASSERT_NE(assertLine, std::string::npos) << out;
    const auto why = out.find("$.id was null, expected != null", assertLine);
    ASSERT_NE(why, std::string::npos) << out;
    EXPECT_EQ(out.find('\n', assertLine), out.rfind('\n', why))
        << "detail must be on the next line\n"
        << out;
}
