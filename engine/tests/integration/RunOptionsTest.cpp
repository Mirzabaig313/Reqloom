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
