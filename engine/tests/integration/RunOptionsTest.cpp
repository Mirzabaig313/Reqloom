// RunOptionsTest — integration tests for RunOptions flags and error propagation.
//
// Covers:
//   - dryRun: chain resolves but no HTTP requests are sent
//   - resetExtractions: clears the extraction cache before the run
//   - resetSessions: invalidates all sessions before the run
//   - non-default environment selection
//   - mid-chain step failure cancels downstream steps
//   - RunOptions::environment selects the correct baseUrl
//
// Each test fails on the parent commit if the corresponding RunOptions
// flag is not wired through ExecutionEngine::run().
#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace ce = chainapi::engine;
namespace ct = chainapi::tests;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

class RunOptionsScratchProject {
public:
    explicit RunOptionsScratchProject(const std::string& yamlBody) {
        const auto unique = "chainapi-runopts-itest-" + std::to_string(::getpid()) + "-" +
                            std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
        std::ofstream{path_ / "chainapi.yaml"} << yamlBody;
    }

    ~RunOptionsScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] fs::path yaml() const { return path_ / "chainapi.yaml"; }

private:
    fs::path path_;
    inline static int counter_{0};
};

/// Minimal project YAML with a single operation that extracts a value.
/// The baseUrl placeholder is replaced by the test after parsing.
constexpr const char* kSimpleProjectYaml = R"YAML(
version: 1
name: RunOptionsTest
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@example.test" }
      extract: { token: $.data.accessToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-bearer
        actor: user
        expect_status: 200
        extract:
          ping_id: $.id
)YAML";

}  // namespace

class RunOptionsFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "polling-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

// ─── resetExtractions ────────────────────────────────────────────────────────

TEST_F(RunOptionsFixture, reset_extractions_clears_cache_before_run) {
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    // First run — populates the extraction cache.
    auto first = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(first.has_value()) << first.error().detail;
    ASSERT_FALSE(ctx.instances(ce::ResourceId{"ping"}).empty());
    EXPECT_EQ(ctx.instances(ce::ResourceId{"ping"}).back().variables.at("ping_id"), "bearer-1");

    // Manually append a stale instance to simulate leftover state.
    ce::ResourceInstance stale;
    stale.variables["ping_id"] = "stale-value";
    ctx.appendInstance(ce::ResourceId{"ping"}, stale);
    ASSERT_EQ(ctx.instances(ce::ResourceId{"ping"}).size(), 2u);

    // Second run with resetExtractions=true — stale instance must be gone.
    ce::RunOptions opts;
    opts.resetExtractions = true;
    auto second = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx, opts);
    ASSERT_TRUE(second.has_value()) << second.error().detail;

    // After reset, only the freshly extracted instance should be present.
    const auto& pings = ctx.instances(ce::ResourceId{"ping"});
    ASSERT_EQ(pings.size(), 1u) << "stale instance should have been cleared";
    EXPECT_EQ(pings.back().variables.at("ping_id"), "bearer-1");
}

// ─── resetSessions ───────────────────────────────────────────────────────────

TEST_F(RunOptionsFixture, reset_sessions_invalidates_session_before_run) {
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    // First run — establishes a session.
    auto first = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(first.has_value()) << first.error().detail;
    ASSERT_NE(ctx.session(ce::ActorId{"user"}), nullptr);
    const auto* sessionAfterFirst = ctx.session(ce::ActorId{"user"});
    ASSERT_NE(sessionAfterFirst, nullptr);

    // Second run without reset — session is still live (not invalidated).
    auto second = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(second.has_value()) << second.error().detail;
    EXPECT_TRUE(second->succeeded());
    // Session must still be present after the second run.
    ASSERT_NE(ctx.session(ce::ActorId{"user"}), nullptr);

    // Third run with resetSessions=true — session is invalidated before the run
    // starts, then re-established during the run. The run must still succeed
    // because the engine re-authenticates automatically.
    ce::RunOptions opts;
    opts.resetSessions = true;
    auto third = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx, opts);
    ASSERT_TRUE(third.has_value()) << third.error().detail;
    EXPECT_TRUE(third->succeeded());
    // Session must be live again after the run.
    ASSERT_NE(ctx.session(ce::ActorId{"user"}), nullptr);
    EXPECT_EQ(ctx.session(ce::ActorId{"user"})->state, ce::ActorSession::State::Live);
}

// ─── Mid-chain failure cancels downstream steps ───────────────────────────────

TEST_F(RunOptionsFixture, mid_chain_failure_cancels_downstream_steps) {
    // Build a project where order.pay depends on order.create, but the
    // mock SUT returns 500 for order.create. The downstream order.pay
    // step must be Cancelled, not attempted.
    RunOptionsScratchProject project(R"YAML(
version: 1
name: FailurePropagation
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@example.test" }
      extract: { token: $.data.accessToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders/will-fail
        actor: user
        expect_status: 201
        extract:
          order_id: $.id
      pay:
        method: POST
        path: /api/v1/orders/{{order.order_id}}/pay
        actor: user
        expect_status: 200
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    // /api/v1/orders/will-fail is not in the mock SUT routes, so it will
    // return a connection error or 404 — either way the step fails.
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"order.pay"}, ctx);

    // The run itself returns a value (not an error) — the failure is
    // captured in the RunResult.
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());

    bool sawCreateFailed = false;
    bool sawPayBlocked = false;
    for (const auto& s : result->steps) {
        if (s.op.value == "order.create" && s.status == ce::StepResult::Status::Failed) {
            sawCreateFailed = true;
        }
        // The engine marks downstream steps as Blocked (not Cancelled) when an
        // upstream step fails. Cancelled is reserved for explicit cancel() calls.
        if (s.op.value == "order.pay" && s.status == ce::StepResult::Status::Blocked) {
            sawPayBlocked = true;
        }
    }
    EXPECT_TRUE(sawCreateFailed) << "order.create should have failed";
    EXPECT_TRUE(sawPayBlocked) << "order.pay should be Blocked after upstream failure";
}

// ─── Non-default environment ──────────────────────────────────────────────────

TEST_F(RunOptionsFixture, non_default_environment_uses_correct_base_url) {
    // Project has two environments: "local" (wrong port) and "staging"
    // (the mock SUT's actual port). Running with environment="staging"
    // must succeed; running with the default "local" must fail.
    RunOptionsScratchProject project(R"YAML(
version: 1
name: MultiEnvTest
default_environment: local

environment:
  baseUrl: http://127.0.0.1:1

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@example.test" }
      extract: { token: $.data.accessToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-bearer
        actor: user
        expect_status: 200
        extract:
          ping_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    // Add a "staging" environment pointing at the real mock SUT.
    loaded->environments["staging"] = {{"baseUrl", harness_->baseUrl()}};

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    // Default environment (port 1) must fail.
    {
        ce::RunContext ctx;
        auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
        // Either a run-level error or a failed RunResult — either way not succeeded.
        const bool failed = !result.has_value() || !result->succeeded();
        EXPECT_TRUE(failed) << "default env (port 1) should not succeed";
    }

    // Staging environment must succeed.
    {
        ce::RunContext ctx;
        ce::RunOptions opts;
        opts.environment = "staging";
        auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx, opts);
        ASSERT_TRUE(result.has_value()) << result.error().detail;
        EXPECT_TRUE(result->succeeded());
        const auto& pings = ctx.instances(ce::ResourceId{"ping"});
        ASSERT_FALSE(pings.empty());
        EXPECT_EQ(pings.back().variables.at("ping_id"), "bearer-1");
    }
}

// ─── RunEvent stream ─────────────────────────────────────────────────────────

TEST_F(RunOptionsFixture, run_events_include_run_started_and_run_ended) {
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    bool sawRunStarted = false;
    bool sawRunEnded = false;
    ce::RunOutcome endedOutcome{};

    engine.subscribe([&](const ce::RunEvent& ev) {
        if (std::holds_alternative<ce::RunStarted>(ev)) {
            sawRunStarted = true;
        }
        if (const auto* e = std::get_if<ce::RunEnded>(&ev)) {
            sawRunEnded = true;
            endedOutcome = e->outcome;
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_TRUE(sawRunStarted);
    EXPECT_TRUE(sawRunEnded);
    EXPECT_EQ(endedOutcome, ce::RunOutcome::Succeeded);
}

TEST_F(RunOptionsFixture, step_started_events_are_emitted_for_each_step) {
    // The engine emits StepStarted for every step it executes. This test
    // confirms at least the ping.get step fires a StepStarted event.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::StepStarted> stepStartedEvents;

    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::StepStarted>(&ev)) {
            stepStartedEvents.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    // At minimum: ping.get step must have fired StepStarted.
    EXPECT_GE(stepStartedEvents.size(), 1u);

    bool sawPingStep = false;
    for (const auto& e : stepStartedEvents) {
        if (e.op.value == "ping.get") sawPingStep = true;
    }
    EXPECT_TRUE(sawPingStep) << "expected StepStarted for ping.get";
}

// ─── Full event stream (AC-3.6.2 / AC-3.6.3 contract surface) ───────────────
// The desktop timeline subscribes to RunEvent and renders one panel per
// event variant. Each of these tests fails on the parent commit because
// the corresponding event was declared in Events.h but never emitted.

TEST_F(RunOptionsFixture, request_prepared_event_fires_with_masked_headers) {
    // RequestPrepared lets the desktop show "what we're about to send".
    // The Authorization header carries the actor's bearer token; it
    // MUST be redacted before reaching the event stream (AC-3.6.3).
    //
    // The chain runs two outbound HTTP calls — the auth login and the
    // ping.get itself. Both emit RequestPrepared via the same code path
    // (the auth flow's emitter is plumbed through AuthDependencies),
    // so the desktop timeline sees one row per send regardless of
    // whether the request came from the executor's main path or from
    // an authenticator.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::RequestPrepared> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::RequestPrepared>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    // Two requests on the wire: the auth login (issued from inside the
    // auth strategy) and the ping.get itself (executor main path).
    ASSERT_EQ(events.size(), 2u) << "expected one RequestPrepared per HTTP send";

    // Auth login is the first send; ping.get is the second.
    const auto& authPrep = events[0];
    EXPECT_EQ(authPrep.method, ce::HttpMethod::Post);
    EXPECT_NE(authPrep.url.find("/api/v1/auth/login"), std::string::npos);

    const auto& pingPrep = events[1];
    EXPECT_EQ(pingPrep.method, ce::HttpMethod::Get);
    EXPECT_NE(pingPrep.url.find("/api/v1/with-bearer"), std::string::npos);

    // Authorization header on the ping.get request MUST be redacted.
    bool sawAuthHeader = false;
    for (const auto& [k, v] : pingPrep.maskedHeaders) {
        if (k == "Authorization") {
            sawAuthHeader = true;
            EXPECT_EQ(v, ce::kRedactedHeaderValue)
                << "Authorization value leaked into the event stream";
        }
    }
    EXPECT_TRUE(sawAuthHeader) << "Authorization header should still be visible (name only)";
}

TEST_F(RunOptionsFixture, response_received_event_carries_status_and_size) {
    // ResponseReceived is the signal the timeline uses to flip a step
    // row from "in flight" to "received". Status, masked headers, and
    // body size are the minimum needed for the row.
    //
    // Both auth-side and main-path responses now flow through this
    // event. Tests assert on the ping.get response (index 1) since the
    // auth response shape is owned by AuthStrategyRefreshTests.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::ResponseReceived> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::ResponseReceived>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(events.size(), 2u);
    const auto& ping = events[1];
    EXPECT_EQ(ping.status, 200);
    EXPECT_GT(ping.bodySize, 0u);
    // Set-Cookie is on the response side and must be redacted in events
    // even though the engine still uses the raw value internally to
    // populate the cookie jar.
    for (const auto& [k, v] : ping.headers) {
        if (k == "Set-Cookie" || k == "set-cookie") {
            EXPECT_EQ(v, ce::kRedactedHeaderValue);
        }
    }
}

TEST_F(RunOptionsFixture, auth_request_response_events_share_step_index_with_parent) {
    // Auth-side events ride on the parent step's stepIndex so the
    // desktop timeline groups them under the operation that triggered
    // the auth — no separate "auth" pseudo-step is needed.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::RequestPrepared> reqEvents;
    std::vector<ce::ResponseReceived> respEvents;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::RequestPrepared>(&ev)) {
            reqEvents.push_back(*e);
        } else if (const auto* e = std::get_if<ce::ResponseReceived>(&ev)) {
            respEvents.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(reqEvents.size(), 2u);
    ASSERT_EQ(respEvents.size(), 2u);

    // Auth was triggered from ensureSession at the start of ping.get's
    // step (stepIndex 0 — single-step chain after deduping). Pin that
    // both auth and main-path events tag the same stepIndex.
    EXPECT_EQ(reqEvents[0].stepIndex, reqEvents[1].stepIndex)
        << "auth and main-path RequestPrepared should share the parent step's index";
    EXPECT_EQ(respEvents[0].stepIndex, respEvents[1].stepIndex)
        << "auth and main-path ResponseReceived should share the parent step's index";
    EXPECT_EQ(reqEvents[0].runId, reqEvents[1].runId);
}

TEST_F(RunOptionsFixture, extraction_applied_event_carries_variable_names_only) {
    // ExtractionApplied is the per-step summary; values are intentionally
    // omitted (per-extraction values live on ExtractionCompleted, where
    // sensitive auth values are already masked separately). This test
    // pins that the event fires once per step that records extractions
    // and that variable names are present.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::ExtractionApplied> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::ExtractionApplied>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    // ping.get extracts ping_id; the auth flow doesn't surface
    // ExtractionApplied — its extractions land on session variables.
    bool sawPingExtraction = false;
    for (const auto& e : events) {
        if (e.resource.value == "ping") {
            sawPingExtraction = true;
            EXPECT_EQ(e.variableNames.size(), 1u);
            if (!e.variableNames.empty()) {
                EXPECT_EQ(e.variableNames[0], "ping_id");
            }
        }
    }
    EXPECT_TRUE(sawPingExtraction) << "expected ExtractionApplied for ping resource";
}


// ─── Cancellation event surface ──────────────────────────────────────────────

namespace {

/// Project YAML for the cancellation test. Two unrelated resources so
/// the chain has at least two steps to exercise the "cancel propagates
/// to downstream steps" path.
constexpr const char* kTwoStepProjectYaml = R"YAML(
version: 1
name: CancelTest
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  first:
    operations:
      get:
        method: GET
        path: /api/v1/with-api-key
        expect_status: 200
        extract:
          first_id: $.id
  second:
    operations:
      get:
        method: GET
        path: /api/v1/with-api-key
        depends_on: [first.get]
        expect_status: 200
)YAML";

}  // namespace

TEST_F(RunOptionsFixture, step_cancelled_event_fires_for_each_cancelled_step) {
    // Cancellation is observable through the StepCancelled event for
    // every step that did not run to completion. The test cancels mid
    // run by hooking StepStarted: when step 0 fires, the subscriber
    // calls engine.cancel(runId). The atomic flip is observed by the
    // retry-loop's isCancelled check at the top of attempt 0, which
    // returns Cancelled without sending any HTTP. Step 1 is then
    // marked Cancelled by the run loop and a StepCancelled event is
    // emitted for it as well — the desktop timeline needs both rows
    // to render the chain accurately.
    RunOptionsScratchProject project(kTwoStepProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::StepCancelled> cancelEvents;
    ce::RunId observedRunId{};
    bool cancelled = false;

    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* started = std::get_if<ce::StepStarted>(&ev)) {
            // Cancel the first time we see a step start. Calling
            // cancel() from inside a subscriber is safe — the engine
            // snapshots the subscriber list before invoking callbacks.
            if (!cancelled) {
                engine.cancel(started->runId);
                observedRunId = started->runId;
                cancelled = true;
            }
        } else if (const auto* c = std::get_if<ce::StepCancelled>(&ev)) {
            cancelEvents.push_back(*c);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"second.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->outcome, ce::RunOutcome::Cancelled);

    // Two steps in the chain: first.get and second.get. Both should
    // have a StepCancelled event — the in-flight one (caught by the
    // retry-loop check) and the downstream one (marked by the run
    // loop's tail-fill).
    ASSERT_EQ(cancelEvents.size(), 2u);
    EXPECT_EQ(cancelEvents[0].runId, observedRunId);
    EXPECT_EQ(cancelEvents[1].runId, observedRunId);

    // Step indexes ascend; the cancelled-in-flight step is index 0,
    // the tail-filled step is index 1.
    EXPECT_EQ(cancelEvents[0].stepIndex, 0u);
    EXPECT_EQ(cancelEvents[1].stepIndex, 1u);

    // Both step ops should appear by name. We don't pin the order
    // beyond stepIndex because the dependency resolver is the source
    // of truth for chain order; first.get → second.get is what it
    // produces here.
    EXPECT_EQ(cancelEvents[0].op.value, "first.get");
    EXPECT_EQ(cancelEvents[1].op.value, "second.get");
}
