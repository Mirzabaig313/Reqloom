// Renderer unit tests — exercise the three CLI output formats against
// hand-crafted RunResult fixtures. Each test asserts on substrings
// rather than full-document equality so the schema can evolve without
// rewriting every fixture, but the contract bits CI consumers depend on
// (status counts, error codes, JUnit attributes) are pinned.

#include "output/JUnitRenderer.h"
#include "output/JsonRenderer.h"
#include "output/TextRenderer.h"

#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>

namespace ce = chainapi::engine;
namespace cli = chainapi::cli;

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
    EXPECT_NE(xml.find("chainapi.target"), std::string::npos);
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
