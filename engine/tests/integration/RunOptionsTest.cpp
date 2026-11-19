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
#include "support/HermeticDependencies.h"

#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "infrastructure/storage/SqliteHistoryStore.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

class CapturingHttpClient final : public ce::HttpClient {
public:
    void enqueue(const int status, std::string body) {
        ce::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        responses_.push_back(std::move(response));
    }

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests_.push_back(request);
        if (responseIndex_ >= responses_.size()) {
            ce::HttpResponse response;
            response.status = 500;
            return response;
        }
        return responses_[responseIndex_++];
    }

    [[nodiscard]] const std::vector<ce::HttpRequest>& requests() const noexcept {
        return requests_;
    }

private:
    std::vector<ce::HttpRequest> requests_;
    std::vector<ce::HttpResponse> responses_;
    std::size_t responseIndex_{};
};

[[nodiscard]] ce::ExecutionEngine makeCapturingEngine(CapturingHttpClient*& captured) {
    auto http = std::make_unique<CapturingHttpClient>();
    captured = http.get();
    ce::ExecutionEngine::Dependencies dependencies{
        std::move(http), nullptr, nullptr, nullptr, nullptr};
    dependencies.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    return ce::ExecutionEngine{std::move(dependencies)};
}

/// Secret keys are namespaced `<project id>/<name>`.
constexpr const char* kHostSecretKey = "hook-secret-hostname/HOST";

class FixedSecretStore final : public ce::SecretStore {
public:
    explicit FixedSecretStore(std::string value) : value_(std::move(value)) {}

    [[nodiscard]] std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        if (name == kHostSecretKey) {
            return std::optional<std::string>{value_};
        }
        return std::optional<std::string>{};
    }

    [[nodiscard]] std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                              const std::string& value) override {
        if (name == kHostSecretKey) {
            value_ = value;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        if (name == kHostSecretKey) {
            value_.clear();
        }
        return {};
    }

private:
    std::string value_;
};

class SecretHostnameHookRunner final : public ce::HookRunner {
public:
    std::expected<ce::HookOutcome, ce::ReqloomError> runPreRequest(
        const std::string&, ce::HookContext context) override {
        const auto secret = context.secrets.find("HOST");
        if (secret == context.secrets.end()) {
            return std::unexpected(ce::ReqloomError{
                ce::ErrorCode::HookFailure, ce::ErrorClass::Hook, "HOST was not loaded"});
        }
        context.request.url = "https://" + secret->second + "/private-path";
        return ce::HookOutcome{std::move(context.request), std::move(context.response)};
    }

    std::expected<ce::HookOutcome, ce::ReqloomError> runPostResponse(
        const std::string&, ce::HookContext context) override {
        return ce::HookOutcome{std::move(context.request), std::move(context.response)};
    }
};

class RunOptionsScratchProject {
public:
    explicit RunOptionsScratchProject(const std::string& yamlBody) {
        path_ = ct::uniqueTempPath("reqloom-runopts-itest");
        fs::create_directories(path_);
        std::ofstream{path_ / "reqloom.yaml"} << yamlBody;
    }

    ~RunOptionsScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] fs::path yaml() const { return path_ / "reqloom.yaml"; }

private:
    fs::path path_;
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

constexpr const char* kUnresolvedRequestFieldsYaml = R"YAML(
version: 1
name: UnresolvedRequestFields

default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  order:
    operations:
      create:
        method: POST
        path: "/api/v1/{{env.PATH}}?raw={{env.RAW}}#{{env.FRAGMENT}}"
        headers: { X-Diagnostic: "{{env.HEADER}}" }
        query_params: { account: "{{env.NAMED}}" }
        body: { value: "{{env.BODY}}" }
        expect_status: 201
)YAML";

constexpr const char* kUnresolvedFormFieldYaml = R"YAML(
version: 1
name: UnresolvedFormField

default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders
        body_form: { quantity: "{{env.FORM}}" }
        expect_status: 201
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

namespace {

void expectEnvironmentDiagnostic(const ce::StepFailed& failure,
                                 const ce::VariableUseKind useKind,
                                 const std::string& useName,
                                 const std::string& token,
                                 const std::string& sourceField) {
    SCOPED_TRACE(token);
    const ce::UnresolvedVariableDiagnostic* match{};
    std::size_t matchCount{};
    for (const auto& diagnostic : failure.diagnostics) {
        if (diagnostic.useKind == useKind && diagnostic.useName == useName &&
            diagnostic.token == token && diagnostic.sourceField == sourceField) {
            match = &diagnostic;
            ++matchCount;
        }
    }

    ASSERT_EQ(matchCount, 1u);
    ASSERT_NE(match, nullptr);
    EXPECT_EQ(match->cause, ce::UnresolvedVariableCause::EnvironmentValueMissing);
    EXPECT_EQ(match->sourceKind, ce::VariableSourceKind::Environment);
    EXPECT_EQ(match->sourceId, "local");
    EXPECT_EQ(match->sourceField, sourceField);
    EXPECT_FALSE(match->producerOp.has_value());
    EXPECT_FALSE(match->producerStepIndex.has_value());
}

}  // namespace

// ─── resetExtractions ────────────────────────────────────────────────────────

TEST_F(RunOptionsFixture, reset_extractions_clears_cache_before_run) {
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
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
    // The in-memory transport returns a forced 500 for order.create after
    // a successful login, so the downstream order.pay request must be blocked.
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    std::vector<ce::StepBlocked> blockedEvents;
    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    http->enqueue(200, R"({"data":{"accessToken":"token"}})");
    http->enqueue(500, R"({"error":"forced failure"})");
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* blocked = std::get_if<ce::StepBlocked>(&event)) {
            blockedEvents.push_back(*blocked);
        }
    });

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

    ASSERT_EQ(blockedEvents.size(), 1u);
    const auto& blocked = blockedEvents.front();
    ASSERT_LT(blocked.stepIndex, result->steps.size());
    ASSERT_LT(blocked.blockedByStepIndex, result->steps.size());
    EXPECT_EQ(result->steps[blocked.stepIndex].op.value, "order.pay");
    EXPECT_EQ(result->steps[blocked.stepIndex].status, ce::StepResult::Status::Blocked);
    EXPECT_EQ(result->steps[blocked.blockedByStepIndex].op.value, "order.create");
    EXPECT_EQ(result->steps[blocked.blockedByStepIndex].status, ce::StepResult::Status::Failed);

    ASSERT_EQ(http->requests().size(), 2u);
    EXPECT_NE(http->requests()[0].url.find("/api/v1/auth/login"), std::string::npos);
    EXPECT_NE(http->requests()[1].url.find("/api/v1/orders/will-fail"), std::string::npos);
    for (const auto& request : http->requests()) {
        EXPECT_EQ(request.url.find("/pay"), std::string::npos);
    }
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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

TEST_F(RunOptionsFixture, unresolved_non_actor_input_fails_before_actor_authentication) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: PreflightBeforeAuthentication
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
      get:
        method: GET
        path: /api/v1/orders/{{env.MISSING_ORDER_ID}}
        actor: user
        expect_status: 200
)YAML");
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);

    ce::RunContext context;
    const auto result = engine.run(*loaded, ce::OperationId{"order.get"}, context);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_TRUE(http->requests().empty());
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().error, ce::ErrorCode::VarUnresolved);
}

TEST_F(RunOptionsFixture, unresolved_path_reports_token_location_and_cause) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: UnresolvedPathDiagnostic
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  order:
    operations:
      get:
        method: GET
        path: /api/v1/orders/{{order.order_id}}
        expect_status: 200
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    std::vector<ce::StepFailed> failedEvents;
    std::size_t preparedRequestCount{};
    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&ev)) {
            failedEvents.push_back(*failed);
        } else if (std::holds_alternative<ce::RequestPrepared>(ev)) {
            ++preparedRequestCount;
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"order.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(preparedRequestCount, 0u);
    EXPECT_TRUE(http->requests().empty());
    ASSERT_EQ(failedEvents.size(), 1u);

    const auto& failure = failedEvents.front();
    const std::string expectedDetail =
        "Variable: {{order.order_id}}\n"
        "Location: URL path\n"
        "Cause: No usable value was available in the current run when this request was prepared.";
    EXPECT_EQ(failure.code, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(failure.detail, expectedDetail);

    const ce::StepResult* failedStep{};
    for (const auto& step : result->steps) {
        if (step.status == ce::StepResult::Status::Failed) {
            failedStep = &step;
            break;
        }
    }
    ASSERT_NE(failedStep, nullptr);
    EXPECT_EQ(failedStep->error, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(failedStep->detail, failure.detail);
}

TEST_F(RunOptionsFixture, unresolved_query_reports_safe_token_and_parameter_location) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: UnresolvedQueryDiagnostic
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  order:
    operations:
      get:
        method: GET
        path: /api/v1/orders
        query_params:
          "organization\r\nid": "{{order.\r\norder_id}}"
        expect_status: 200
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    std::vector<ce::StepFailed> failedEvents;
    std::size_t preparedRequestCount{};
    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&ev)) {
            failedEvents.push_back(*failed);
        } else if (std::holds_alternative<ce::RequestPrepared>(ev)) {
            ++preparedRequestCount;
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"order.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(preparedRequestCount, 0u);
    EXPECT_TRUE(http->requests().empty());
    ASSERT_EQ(failedEvents.size(), 1u);

    const std::string expectedDetail =
        "Variable: {{order.\\x0D\\x0Aorder_id}}\n"
        "Location: Query parameter \"organization\\x0D\\x0Aid\"\n"
        "Cause: No usable value was available in the current run when this request was prepared.";
    EXPECT_EQ(failedEvents.front().code, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(failedEvents.front().detail, expectedDetail);

    const ce::StepResult* failedStep{};
    for (const auto& step : result->steps) {
        if (step.status == ce::StepResult::Status::Failed) {
            failedStep = &step;
            break;
        }
    }
    ASSERT_NE(failedStep, nullptr);
    EXPECT_EQ(failedStep->error, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(failedStep->detail, failedEvents.front().detail);
}

TEST_F(RunOptionsFixture, unresolved_request_fields_report_all_structured_diagnostics) {
    RunOptionsScratchProject project(kUnresolvedRequestFieldsYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    std::vector<ce::StepFailed> failedEvents;
    std::size_t preparedRequestCount{};
    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&event)) {
            failedEvents.push_back(*failed);
        } else if (std::holds_alternative<ce::RequestPrepared>(event)) {
            ++preparedRequestCount;
        }
    });

    ce::RunContext ctx;
    const auto result = engine.run(*loaded, ce::OperationId{"order.create"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(preparedRequestCount, 0u);
    EXPECT_TRUE(http->requests().empty());
    ASSERT_EQ(failedEvents.size(), 1u);
    const auto& failure = failedEvents.front();
    EXPECT_EQ(failure.code, ce::ErrorCode::VarUnresolved);
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().status, ce::StepResult::Status::Failed);
    EXPECT_EQ(result->steps.front().error, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(result->steps.front().detail, failure.detail);
    ASSERT_EQ(failure.diagnostics.size(), 6u);
    expectEnvironmentDiagnostic(failure, ce::VariableUseKind::UrlPath, "", "env.PATH", "PATH");
    expectEnvironmentDiagnostic(failure, ce::VariableUseKind::RawQuery, "", "env.RAW", "RAW");
    expectEnvironmentDiagnostic(
        failure, ce::VariableUseKind::Fragment, "", "env.FRAGMENT", "FRAGMENT");
    expectEnvironmentDiagnostic(
        failure, ce::VariableUseKind::NamedQuery, "account", "env.NAMED", "NAMED");
    expectEnvironmentDiagnostic(
        failure, ce::VariableUseKind::Header, "X-Diagnostic", "env.HEADER", "HEADER");
    expectEnvironmentDiagnostic(failure, ce::VariableUseKind::Body, "", "env.BODY", "BODY");
}

TEST_F(RunOptionsFixture, unresolved_form_field_reports_structured_diagnostic) {
    RunOptionsScratchProject project(kUnresolvedFormFieldYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    std::vector<ce::StepFailed> failedEvents;
    std::size_t preparedRequestCount{};
    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&event)) {
            failedEvents.push_back(*failed);
        } else if (std::holds_alternative<ce::RequestPrepared>(event)) {
            ++preparedRequestCount;
        }
    });

    ce::RunContext ctx;
    const auto result = engine.run(*loaded, ce::OperationId{"order.create"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(preparedRequestCount, 0u);
    EXPECT_TRUE(http->requests().empty());
    ASSERT_EQ(failedEvents.size(), 1u);
    const auto& failure = failedEvents.front();
    EXPECT_EQ(failure.code, ce::ErrorCode::VarUnresolved);
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().status, ce::StepResult::Status::Failed);
    EXPECT_EQ(result->steps.front().error, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(result->steps.front().detail, failure.detail);
    ASSERT_EQ(failure.diagnostics.size(), 1u);
    expectEnvironmentDiagnostic(
        failure, ce::VariableUseKind::FormField, "quantity", "env.FORM", "FORM");
}

TEST_F(RunOptionsFixture, step_started_events_are_emitted_for_each_step) {
    // The engine emits StepStarted for every step it executes. This test
    // confirms at least the ping.get step fires a StepStarted event.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

TEST_F(RunOptionsFixture, request_prepared_event_fires_with_real_headers_on_the_desktop) {
    // RequestPrepared lets the desktop show "what we're about to send". Owner decision
    // 2026-11-19: a desktop run shows the real URL and the real Authorization value (the actor's
    // session token is not a declared secret); an agent run masks both, as AC-3.6.3 required of
    // every run before. The agent half is pinned below.
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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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
    EXPECT_NE(authPrep.url.find("/api/v1/auth/login"), std::string::npos) << authPrep.url;

    const auto& pingPrep = events[1];
    EXPECT_EQ(pingPrep.method, ce::HttpMethod::Get);
    EXPECT_NE(pingPrep.url.find("/api/v1/with-bearer"), std::string::npos) << pingPrep.url;

    // The person sees the bearer token that was sent.
    bool sawAuthHeader = false;
    for (const auto& [k, v] : pingPrep.maskedHeaders) {
        if (k == "Authorization") {
            sawAuthHeader = true;
            EXPECT_TRUE(v.starts_with("Bearer ")) << v;
            EXPECT_NE(v, ce::kRedactedHeaderValue);
        }
    }
    EXPECT_TRUE(sawAuthHeader);

    // An agent run of the same chain: URL opaque, Authorization masked.
    ce::ExecutionEngine agentEngine(
        reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
    std::vector<ce::RequestPrepared> agentEvents;
    agentEngine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::RequestPrepared>(&ev)) {
            agentEvents.push_back(*e);
        }
    });
    ce::RunContext agentCtx;
    ce::RunOptions agent;
    agent.agentOutput = true;
    ASSERT_TRUE(agentEngine.run(*loaded, ce::OperationId{"ping.get"}, agentCtx, agent).has_value());
    ASSERT_EQ(agentEvents.size(), 2u);
    EXPECT_EQ(agentEvents[0].url.find("/api/v1/auth/login"), std::string::npos);
    EXPECT_NE(agentEvents[1].url.find("/%3Credacted%3E"), std::string::npos);
    for (const auto& [k, v] : agentEvents[1].maskedHeaders) {
        if (k == "Authorization") {
            EXPECT_EQ(v, ce::kRedactedHeaderValue) << "Authorization leaked to an agent run";
        }
    }
}

TEST_F(RunOptionsFixture, request_prepared_redacts_url_components_without_changing_wire_url) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: OpaqueRequestUrl

default_environment: local

environment:
  baseUrl: http://wire-user:wire-password@placeholder
  PATH: wire-path
  KEY: wire-key
  QUERY: wire-query
  FRAGMENT: wire-fragment

resources:
  ping:
    operations:
      get:
        method: GET
        path: "/{{env.PATH}}?{{env.KEY}}={{env.QUERY}}#{{env.FRAGMENT}}"
        expect_status: 200
)YAML");
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    http->enqueue(200, "{}");
    std::vector<ce::RequestPrepared> events;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            events.push_back(*prepared);
        }
    });

    ce::RunContext context;
    const auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, context);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(http->requests().size(), 1u);
    EXPECT_EQ(
        http->requests().front().url,
        "http://wire-user:wire-password@placeholder/wire-path?wire-key=wire-query#wire-fragment");
    ASSERT_EQ(events.size(), 1u);
    // Desktop (owner decision 2026-11-19): the real URL, with only the userinfo password masked.
    EXPECT_EQ(
        events.front().url,
        "http://wire-user:<redacted>@placeholder/wire-path?wire-key=wire-query#wire-fragment");

    // An agent run keeps every component opaque.
    CapturingHttpClient* agentHttp{};
    auto agentEngine = makeCapturingEngine(agentHttp);
    agentHttp->enqueue(200, "{}");
    std::vector<ce::RequestPrepared> agentEvents;
    agentEngine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            agentEvents.push_back(*prepared);
        }
    });
    ce::RunContext agentContext;
    ce::RunOptions agent;
    agent.agentOutput = true;
    ASSERT_TRUE(
        agentEngine.run(*loaded, ce::OperationId{"ping.get"}, agentContext, agent).has_value());
    ASSERT_EQ(agentEvents.size(), 1u);
    EXPECT_EQ(agentEvents.front().url,
              "http://%3Credacted%3E/%3Credacted%3E?%3Credacted%3E#%3Credacted%3E");
    for (const char* part : {"placeholder",
                             "wire-user",
                             "wire-password",
                             "wire-path",
                             "wire-key",
                             "wire-query",
                             "wire-fragment"}) {
        EXPECT_EQ(agentEvents.front().url.find(part), std::string::npos) << part;
    }
}

TEST_F(RunOptionsFixture, request_prepared_redacts_hook_secret_hostname_in_events_and_history) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: HookSecretHostname
id: hook-secret-hostname

default_environment: local

environment:
  baseUrl: http://placeholder
  hookHost: !secret HOST

resources:
  ping:
    operations:
      get:
        method: GET
        path: /initial-path
        pre_request: "mutate URL from ctx.secret.HOST"
        expect_status: 200
)YAML");
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    const auto dbPath = ct::uniqueTempPath("reqloom-history-hook-url", ".sqlite");
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);

    auto http = std::make_unique<CapturingHttpClient>();
    auto* capturedHttp = http.get();
    capturedHttp->enqueue(200, "{}");
    auto history = std::make_unique<ce::SqliteHistoryStore>();
    ASSERT_TRUE(history->open(dbPath).has_value());
    ce::ExecutionEngine::Dependencies dependencies{
        std::move(http),
        nullptr,
        std::move(history),
        std::make_unique<FixedSecretStore>("wire-secret.example"),
        std::make_unique<SecretHostnameHookRunner>()};
    // Secrets need the folder check; a scratch trust file keeps it hermetic.
    dependencies.trust =
        ce::makeFileProjectTrustStore(ct::uniqueTempPath("reqloom-trust-hook-url", ".json"));
    dependencies.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(dependencies)};

    std::vector<ce::RequestPrepared> emitted;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            emitted.push_back(*prepared);
        }
    });

    ce::RunContext context;
    const auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, context);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(capturedHttp->requests().size(), 1u);
    EXPECT_EQ(capturedHttp->requests().front().url, "https://wire-secret.example/private-path");
    ASSERT_EQ(emitted.size(), 1u);
    // The host is a declared secret's value: masked even on the desktop; the path is not.
    EXPECT_EQ(emitted.front().url, "https://<secret:HOST>/private-path");
    EXPECT_EQ(emitted.front().url.find("wire-secret.example"), std::string::npos);

    const auto persisted = engine.historyEvents(result->runId);
    ASSERT_TRUE(persisted.has_value()) << persisted.error().detail;
    std::vector<ce::RequestPrepared> persistedRequests;
    for (const auto& event : *persisted) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            persistedRequests.push_back(*prepared);
        }
    }
    ASSERT_EQ(persistedRequests.size(), 1u);
    EXPECT_EQ(persistedRequests.front().url, "https://<secret:HOST>/private-path");
    EXPECT_EQ(persistedRequests.front().url.find("wire-secret.example"), std::string::npos);

    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);
}

TEST_F(RunOptionsFixture, request_prepared_redacts_scheme_like_text_inside_query) {
    RunOptionsScratchProject project(R"YAML(
version: 1
name: SchemeLikeQueryText

default_environment: local

environment:
  baseUrl: ""
  CLIENT_ID: wire-secret

resources:
  oauth:
    operations:
      begin:
        method: GET
        path: "/authorize?client_id={{env.CLIENT_ID}}&redirect_uri=https://app.example.test/cb"
        expect_status: 200
)YAML");
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    CapturingHttpClient* http{};
    auto engine = makeCapturingEngine(http);
    http->enqueue(200, "{}");
    std::vector<ce::RequestPrepared> events;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            events.push_back(*prepared);
        }
    });

    ce::RunContext context;
    const auto result = engine.run(*loaded, ce::OperationId{"oauth.begin"}, context);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(http->requests().size(), 1u);
    EXPECT_EQ(http->requests().front().url,
              "/authorize?client_id=wire-secret&redirect_uri=https://app.example.test/cb");
    ASSERT_EQ(events.size(), 1u);
    // Desktop: CLIENT_ID is an ordinary environment value, so the real URL is shown.
    EXPECT_EQ(events.front().url,
              "/authorize?client_id=wire-secret&redirect_uri=https://app.example.test/cb");

    // An agent run: the scheme-like text inside the query does not defeat the opaque URL.
    CapturingHttpClient* agentHttp{};
    auto agentEngine = makeCapturingEngine(agentHttp);
    agentHttp->enqueue(200, "{}");
    std::vector<ce::RequestPrepared> agentEvents;
    agentEngine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            agentEvents.push_back(*prepared);
        }
    });
    ce::RunContext agentContext;
    ce::RunOptions agent;
    agent.agentOutput = true;
    ASSERT_TRUE(
        agentEngine.run(*loaded, ce::OperationId{"oauth.begin"}, agentContext, agent).has_value());
    ASSERT_EQ(agentEvents.size(), 1u);
    EXPECT_EQ(agentEvents.front().url, "/%3Credacted%3E?%3Credacted%3E&%3Credacted%3E");
    EXPECT_EQ(agentEvents.front().url.find("wire-secret"), std::string::npos);
    EXPECT_EQ(agentEvents.front().url.find("app.example.test"), std::string::npos);
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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

TEST_F(RunOptionsFixture, capture_response_bodies_off_by_default_leaves_body_empty) {
    // Default behavior: bodies stay off the event surface. Every
    // ResponseReceived must carry an empty body optional.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

    std::vector<ce::ResponseReceived> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::ResponseReceived>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_FALSE(events.empty());
    for (const auto& ev : events) {
        EXPECT_FALSE(ev.body.has_value());
    }
}

TEST_F(RunOptionsFixture, capture_response_bodies_includes_auth_and_main_path_bodies) {
    // Opt-in capture: both the auth response (index 0) and the main-path
    // response (index 1) carry their bodies exactly as received, login token included.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

    std::vector<ce::ResponseReceived> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::ResponseReceived>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    ce::RunOptions options;
    options.captureResponseBodies = true;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx, options);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(events.size(), 2u);

    const auto& login = events[0];
    ASSERT_TRUE(login.body.has_value());
    EXPECT_NE(login.body->find("accessToken"), std::string::npos);
    EXPECT_NE(login.body->find("tok-1"), std::string::npos) << *login.body;
    EXPECT_EQ(login.body->find(ce::kRedactedHeaderValue), std::string::npos) << *login.body;
    EXPECT_EQ(login.body->size(), login.bodySize);

    // Main-path response.
    const auto& ping = events[1];
    ASSERT_TRUE(ping.body.has_value());
    EXPECT_NE(ping.body->find("bearer-1"), std::string::npos);
    EXPECT_EQ(ping.body->size(), ping.bodySize);
}

TEST_F(RunOptionsFixture, auth_request_response_events_share_step_index_with_parent) {
    // Auth-side events ride on the parent step's stepIndex so the
    // desktop timeline groups them under the operation that triggered
    // the auth — no separate "auth" pseudo-step is needed.
    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

    std::vector<ce::RequestPrepared> reqEvents;
    std::vector<ce::ResponseReceived> respEvents;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* reqEv = std::get_if<ce::RequestPrepared>(&ev)) {
            reqEvents.push_back(*reqEv);
        } else if (const auto* respEv = std::get_if<ce::ResponseReceived>(&ev)) {
            respEvents.push_back(*respEv);
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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));

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

// ─── HistoryStore end-to-end ────────────────────────────────────────────────
// Confirms the executor's emit() path persists every RunEvent into the
// SQLite store, that the runs table denormalises correctly from the
// stream, and that a second process (simulated by a second store
// instance) can read back what the first wrote.

TEST_F(RunOptionsFixture, run_persists_full_event_stream_to_history_store) {
    const auto dbPath = ct::uniqueTempPath("reqloom-history-itest", ".sqlite");
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);

    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine::Dependencies deps =
        reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies());
    ASSERT_NE(deps.history, nullptr);
    auto opened = deps.history->open(dbPath);
    ASSERT_TRUE(opened.has_value()) << opened.error().detail;

    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine(std::move(deps));
    ce::RunContext ctx;

    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    // Open a SECOND store instance against the same file — same shape
    // the desktop process will use to read the writer's output.
    ce::SqliteHistoryStore reader;
    ASSERT_TRUE(reader.open(dbPath).has_value());

    auto runs = reader.listRuns(10);
    ASSERT_TRUE(runs.has_value()) << runs.error().detail;
    ASSERT_EQ(runs->size(), 1u);
    EXPECT_EQ((*runs)[0].targetOp.value, "ping.get");
    EXPECT_EQ((*runs)[0].outcome, "Succeeded");
    EXPECT_EQ((*runs)[0].envName, "local");

    auto events = reader.eventsFor((*runs)[0].runId);
    ASSERT_TRUE(events.has_value());

    // Every variant we emit on the happy path should be present.
    bool sawRunStarted = false;
    bool sawStepStarted = false;
    bool sawRequestPrepared = false;
    bool sawResponseReceived = false;
    bool sawExtractionApplied = false;
    bool sawRunEnded = false;
    for (const auto& ev : *events) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ce::RunStarted>)
                    sawRunStarted = true;
                else if constexpr (std::is_same_v<T, ce::StepStarted>)
                    sawStepStarted = true;
                else if constexpr (std::is_same_v<T, ce::RequestPrepared>)
                    sawRequestPrepared = true;
                else if constexpr (std::is_same_v<T, ce::ResponseReceived>)
                    sawResponseReceived = true;
                else if constexpr (std::is_same_v<T, ce::ExtractionApplied>)
                    sawExtractionApplied = true;
                else if constexpr (std::is_same_v<T, ce::RunEnded>)
                    sawRunEnded = true;
            },
            ev);
    }
    EXPECT_TRUE(sawRunStarted);
    EXPECT_TRUE(sawStepStarted);
    EXPECT_TRUE(sawRequestPrepared);
    EXPECT_TRUE(sawResponseReceived);
    EXPECT_TRUE(sawExtractionApplied);
    EXPECT_TRUE(sawRunEnded);

    reader.close();
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);
}

TEST_F(RunOptionsFixture, history_store_persists_request_headers_already_masked) {
    // The history pane reads headers directly out of the payload column; the masker that runs
    // at emit time is the only line of defence between the bearer token and the disk. Owner
    // decision 2026-11-19: a desktop run stores the real value (the person's own history); an
    // agent run's stored header stays masked. Pinned here for the agent run.
    const auto dbPath = ct::uniqueTempPath("reqloom-history-mask", ".sqlite");
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);

    RunOptionsScratchProject project(kSimpleProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine::Dependencies deps =
        reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies());
    ASSERT_TRUE(deps.history->open(dbPath).has_value());

    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine(std::move(deps));
    ce::RunContext ctx;
    ce::RunOptions agent;
    agent.agentOutput = true;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx, agent);
    ASSERT_TRUE(result.has_value());

    ce::SqliteHistoryStore reader;
    ASSERT_TRUE(reader.open(dbPath).has_value());
    auto runs = reader.listRuns(10);
    ASSERT_TRUE(runs.has_value() && !runs->empty());

    auto events = reader.eventsFor((*runs)[0].runId);
    ASSERT_TRUE(events.has_value());

    bool sawAuthHeaderRedacted = false;
    for (const auto& ev : *events) {
        if (const auto* req = std::get_if<ce::RequestPrepared>(&ev)) {
            for (const auto& [k, v] : req->maskedHeaders) {
                if (k == "Authorization") {
                    EXPECT_EQ(v, ce::kRedactedHeaderValue)
                        << "Authorization value reached disk unredacted";
                    sawAuthHeaderRedacted = true;
                }
            }
        }
    }
    EXPECT_TRUE(sawAuthHeaderRedacted)
        << "expected at least one persisted RequestPrepared with masked Authorization";

    reader.close();
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);
}

// ─── F1 regression: extracted secret values must not reach disk ─────────────

TEST_F(RunOptionsFixture, extracted_secret_value_is_masked_in_persisted_history) {
    // An op that extracts a secret-named variable (here `session_token`)
    // must NOT write the plaintext value into the persisted
    // ExtractionCompleted event. Masking happens on the event copy only
    // — the RunContext keeps the real value so downstream templating
    // still works. Engine Requirement AC-3.6.3.
    constexpr const char* kTokenExtractYaml = R"YAML(
version: 1
name: TokenExtract
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  sess:
    operations:
      start:
        method: GET
        path: /api/v1/with-bearer
        expect_status: 200
        extract:
          session_token: $.id
)YAML";

    const auto dbPath = ct::uniqueTempPath("reqloom-history-secret", ".sqlite");
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);

    RunOptionsScratchProject project(kTokenExtractYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine::Dependencies deps =
        reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies());
    ASSERT_TRUE(deps.history->open(dbPath).has_value());

    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine(std::move(deps));
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"sess.start"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->succeeded());

    // The real value DID land in the run context (downstream templating
    // depends on it) — the mock returns id "bearer-1".
    const auto& instances = ctx.instances(ce::ResourceId{"sess"});
    ASSERT_FALSE(instances.empty());
    EXPECT_EQ(instances.back().variables.at("session_token"), "bearer-1");

    // But the persisted event copy must be redacted.
    ce::SqliteHistoryStore reader;
    ASSERT_TRUE(reader.open(dbPath).has_value());
    auto runs = reader.listRuns(10);
    ASSERT_TRUE(runs.has_value() && !runs->empty());
    auto events = reader.eventsFor((*runs)[0].runId);
    ASSERT_TRUE(events.has_value());

    bool sawTokenExtraction = false;
    for (const auto& ev : *events) {
        if (const auto* ext = std::get_if<ce::ExtractionCompleted>(&ev)) {
            if (ext->variableName == "session_token") {
                sawTokenExtraction = true;
                EXPECT_EQ(ext->outcome, ce::ExtractionCompleted::Outcome::Resolved);
                EXPECT_EQ(ext->value, ce::kRedactedHeaderValue)
                    << "extracted token value reached disk unredacted";
                EXPECT_EQ(ext->value.find("bearer-1"), std::string::npos);
            }
        }
    }
    EXPECT_TRUE(sawTokenExtraction) << "expected a persisted ExtractionCompleted for session_token";

    reader.close();
    fs::remove(dbPath, ec);
    fs::remove(fs::path{dbPath.string() + "-wal"}, ec);
    fs::remove(fs::path{dbPath.string() + "-shm"}, ec);
}

// ─── Retry semantics (D-06), failure detail (D-13), unknown actor (D-04) ────

namespace {

/// Returns a scripted sequence of responses or transport errors; once the
/// script runs out it repeats the last entry.
class ScriptedHttpClient final : public ce::HttpClient {
public:
    void respond(const int status, std::string body = "{}") {
        ce::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        script_.emplace_back(std::move(response));
    }
    void fail(const ce::ErrorCode code) {
        script_.emplace_back(
            std::unexpected(ce::ReqloomError{code, ce::ErrorClass::Network, "scripted failure"}));
    }

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests_.push_back(request);
        if (script_.empty()) {
            return ce::HttpResponse{};
        }
        const auto index = std::min(next_, script_.size() - 1);
        ++next_;
        return script_[index];
    }

    [[nodiscard]] std::size_t sends() const noexcept { return requests_.size(); }

private:
    std::vector<std::expected<ce::HttpResponse, ce::ReqloomError>> script_;
    std::vector<ce::HttpRequest> requests_;
    std::size_t next_{};
};

constexpr const char* kRetryProjectYaml = R"YAML(
version: 1
name: RetrySemantics
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
        retry: { max: 3, backoff: 1 }
      create:
        method: POST
        path: /items
        body: { name: "x" }
        expect_status: 201
        retry: { max: 3, backoff: 1 }
      patch:
        method: PATCH
        path: /items/1
        body: { name: "y" }
        expect_status: 200
        retry: { max: 3, backoff: 1 }
      expects_500:
        method: GET
        path: /boom
        expect_status: 500
        retry: { max: 3, backoff: 1 }
      single:
        method: GET
        path: /items
        expect_status: 200
        retry: { max: 1, backoff: 1 }
)YAML";

struct ScriptedRun {
    std::expected<ce::RunResult, ce::ReqloomError> result;
    std::size_t sends{};
    std::vector<ce::StepFailed> failures;
};

[[nodiscard]] ScriptedRun runScripted(const std::string& yaml,
                                      const std::string& target,
                                      const std::function<void(ScriptedHttpClient&)>& script,
                                      const std::function<void(ce::Project&)>& mutate = {},
                                      const ce::RunOptions& options = {}) {
    RunOptionsScratchProject project(yaml);
    auto loaded = ce::parseProject(project.yaml());
    EXPECT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().detail);
    if (mutate) {
        mutate(*loaded);
    }
    auto http = std::make_unique<ScriptedHttpClient>();
    auto* raw = http.get();
    script(*raw);
    ce::ExecutionEngine::Dependencies dependencies{
        std::move(http), nullptr, nullptr, nullptr, nullptr};
    dependencies.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(dependencies)};
    ScriptedRun out;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&event)) {
            out.failures.push_back(*failed);
        }
    });
    ce::RunContext context;
    out.result = engine.run(*loaded, ce::OperationId{target}, context, options);
    out.sends = raw->sends();
    return out;
}

[[nodiscard]] const ce::StepResult* finalStep(const ce::RunResult& result, std::string_view op) {
    const ce::StepResult* found = nullptr;
    for (const auto& step : result.steps) {
        if (step.op.value == op && !step.pollAttempt) {
            found = &step;
        }
    }
    return found;
}

}  // namespace

TEST(RetrySemantics, max_attempts_is_the_total_number_of_sends_on_transport_errors) {
    const auto run = runScripted(kRetryProjectYaml, "item.read", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkTimeout);
    });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u) << "max: 3 must mean three sends in total, not 1 + 3 retries";
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->status, ce::StepResult::Status::Failed);
    EXPECT_EQ(step->error, ce::ErrorCode::NetworkTimeout);
    EXPECT_EQ(step->attempts, 3);
}

TEST(RetrySemantics, max_attempts_one_sends_exactly_once) {
    const auto run = runScripted(kRetryProjectYaml, "item.single", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkTimeout);
    });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 1u);
}

TEST(RetrySemantics, idempotent_get_retries_5xx_within_budget) {
    const auto run = runScripted(kRetryProjectYaml, "item.read", [](ScriptedHttpClient& http) {
        http.respond(503);
        http.respond(502);
        http.respond(200);
    });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 3u);
    EXPECT_TRUE(run.result->succeeded());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->attempts, 3);
}

TEST(RetrySemantics, persistent_5xx_fails_with_http5xx_after_budget) {
    const auto run = runScripted(
        kRetryProjectYaml, "item.read", [](ScriptedHttpClient& http) { http.respond(503); });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 3u);
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::Http5xx);
    EXPECT_EQ(step->attempts, 3);
}

TEST(RetrySemantics, expected_5xx_is_not_retried) {
    const auto run = runScripted(
        kRetryProjectYaml, "item.expects_500", [](ScriptedHttpClient& http) { http.respond(500); });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 1u);
    EXPECT_TRUE(run.result->succeeded());
}

TEST(RetrySemantics, post_is_not_resent_after_ambiguous_transport_error) {
    const auto run = runScripted(kRetryProjectYaml, "item.create", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkTimeout);
    });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 1u) << "a POST the server may have processed must not be re-sent";
}

TEST(RetrySemantics, post_and_patch_are_not_resent_after_5xx) {
    const auto post = runScripted(
        kRetryProjectYaml, "item.create", [](ScriptedHttpClient& http) { http.respond(503); });
    ASSERT_TRUE(post.result.has_value());
    EXPECT_EQ(post.sends, 1u);
    const auto patch = runScripted(
        kRetryProjectYaml, "item.patch", [](ScriptedHttpClient& http) { http.respond(503); });
    ASSERT_TRUE(patch.result.has_value());
    EXPECT_EQ(patch.sends, 1u);
}

TEST(RetrySemantics, post_retries_when_request_never_left_the_client) {
    const auto run = runScripted(kRetryProjectYaml, "item.create", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkDns);
        http.respond(201);
    });
    ASSERT_TRUE(run.result.has_value());
    EXPECT_EQ(run.sends, 2u);
    EXPECT_TRUE(run.result->succeeded());
}

namespace {

// POST and PATCH declared safe to repeat via `retry.non_idempotent`, next to
// a plain POST without the flag.
constexpr const char* kRepeatableRetryYaml = R"YAML(
version: 1
name: RetryNonIdempotent
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  item:
    operations:
      create_repeatable:
        method: POST
        path: /items
        body: { name: "x" }
        expect_status: 201
        retry: { max: 3, backoff: 1, non_idempotent: true }
      patch_repeatable:
        method: PATCH
        path: /items/1
        body: { name: "y" }
        expect_status: 200
        retry: { max: 3, backoff: 1, non_idempotent: true }
      create:
        method: POST
        path: /items
        body: { name: "x" }
        expect_status: 201
        retry: { max: 3, backoff: 1 }
      read:
        method: GET
        path: /items
        expect_status: 200
        retry: { max: 3, backoff: 1 }
)YAML";

}  // namespace

TEST(RetrySemantics, non_idempotent_flag_retries_post_on_5xx_up_to_max_attempts) {
    const auto run = runScripted(kRepeatableRetryYaml,
                                 "item.create_repeatable",
                                 [](ScriptedHttpClient& http) { http.respond(503); });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u);
    const auto* step = finalStep(*run.result, "item.create_repeatable");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::Http5xx);
    EXPECT_EQ(step->attempts, 3);
}

TEST(RetrySemantics, non_idempotent_flag_retries_post_on_timeout_up_to_max_attempts) {
    const auto run =
        runScripted(kRepeatableRetryYaml, "item.create_repeatable", [](ScriptedHttpClient& http) {
            http.fail(ce::ErrorCode::NetworkTimeout);
        });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u);
    const auto* step = finalStep(*run.result, "item.create_repeatable");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::NetworkTimeout);
    EXPECT_EQ(step->attempts, 3);
}

TEST(RetrySemantics, non_idempotent_flag_retries_patch_and_stops_on_success) {
    const auto run =
        runScripted(kRepeatableRetryYaml, "item.patch_repeatable", [](ScriptedHttpClient& http) {
            http.respond(502);
            http.respond(200);
        });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 2u);
    EXPECT_TRUE(run.result->succeeded());
}

TEST(RetrySemantics, post_without_flag_is_not_resent_on_5xx_or_timeout) {
    const auto on5xx = runScripted(
        kRepeatableRetryYaml, "item.create", [](ScriptedHttpClient& http) { http.respond(503); });
    ASSERT_TRUE(on5xx.result.has_value());
    EXPECT_EQ(on5xx.sends, 1u);
    const auto onTimeout =
        runScripted(kRepeatableRetryYaml, "item.create", [](ScriptedHttpClient& http) {
            http.fail(ce::ErrorCode::NetworkTimeout);
        });
    ASSERT_TRUE(onTimeout.result.has_value());
    EXPECT_EQ(onTimeout.sends, 1u);
}

TEST(RetrySemantics, post_without_flag_is_resent_after_connection_failure) {
    // NetworkConnect means no request bytes reached the server.
    const auto run = runScripted(kRepeatableRetryYaml, "item.create", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkConnect);
        http.fail(ce::ErrorCode::NetworkConnect);
        http.respond(201);
    });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u);
    EXPECT_TRUE(run.result->succeeded());
}

TEST(RetrySemantics, persistent_connection_failure_fails_with_network_connect) {
    const auto run = runScripted(kRepeatableRetryYaml, "item.create", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkConnect);
    });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u);
    const auto* step = finalStep(*run.result, "item.create");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::NetworkConnect);
}

TEST(RetrySemantics, get_retries_connection_failure_like_other_transport_errors) {
    const auto run = runScripted(kRepeatableRetryYaml, "item.read", [](ScriptedHttpClient& http) {
        http.fail(ce::ErrorCode::NetworkConnect);
        http.fail(ce::ErrorCode::NetworkTimeout);
        http.respond(200);
    });
    ASSERT_TRUE(run.result.has_value()) << run.result.error().detail;
    EXPECT_EQ(run.sends, 3u);
    EXPECT_TRUE(run.result->succeeded());
}

// Decided behaviour: the API's error text is quoted for every step, with or
// without body capture, but sensitive fields and secret values are masked.
TEST(FailureDetail, status_mismatch_detail_includes_masked_excerpt_by_default) {
    const auto run = runScripted(kRetryProjectYaml, "item.read", [](ScriptedHttpClient& http) {
        http.respond(400, R"({"error":"API-ERROR-TEXT","token":"SECRET-BODY-VALUE"})");
    });
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::Http4xx);
    EXPECT_NE(step->detail.find("HTTP 400"), std::string::npos) << step->detail;
    EXPECT_NE(step->detail.find("200"), std::string::npos) << "expected status missing";
    EXPECT_NE(step->detail.find("API-ERROR-TEXT"), std::string::npos) << step->detail;
    EXPECT_EQ(step->detail.find("SECRET-BODY-VALUE"), std::string::npos) << step->detail;
    ASSERT_EQ(run.failures.size(), 1u);
    EXPECT_NE(run.failures.front().detail.find("API-ERROR-TEXT"), std::string::npos);
    EXPECT_EQ(run.failures.front().detail.find("SECRET-BODY-VALUE"), std::string::npos);
}

TEST(FailureDetail, status_mismatch_detail_includes_excerpt_when_capture_enabled) {
    ce::RunOptions options;
    options.captureResponseBodies = true;
    const auto run = runScripted(
        kRetryProjectYaml,
        "item.read",
        [](ScriptedHttpClient& http) { http.respond(400, R"({"error":"visible-body"})"); },
        {},
        options);
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("visible-body"), std::string::npos) << step->detail;
}

TEST(FailureDetail, login_step_detail_shows_error_text_with_token_masked) {
    constexpr const char* kLoginYaml = R"YAML(
version: 1
name: LoginDetail
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  session:
    operations:
      login:
        method: POST
        path: /auth/login
        body: { user: "u", password: "p" }
        expect_status: 200
        extract:
          access_token: $.access_token
)YAML";
    const auto run = runScripted(kLoginYaml, "session.login", [](ScriptedHttpClient& http) {
        http.respond(400, R"({"hint":"LOGIN-BODY","access_token":"abc123def456"})");
    });
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "session.login");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->status, ce::StepResult::Status::Failed);
    EXPECT_NE(step->detail.find("LOGIN-BODY"), std::string::npos) << step->detail;
    EXPECT_EQ(step->detail.find("abc123def456"), std::string::npos) << step->detail;
}

TEST(FailureDetail, status_mismatch_detail_quotes_a_long_body_whole) {
    const std::string body =
        R"({"error":"API-ERROR-TEXT","pad":")" + std::string(1500, 'x') + R"(","end":"TAIL"})";
    const auto run = runScripted(
        kRetryProjectYaml, "item.read", [&](ScriptedHttpClient& http) { http.respond(400, body); });
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("TAIL"), std::string::npos) << "a 1.5 KB body is not cut";
    EXPECT_EQ(step->detail.find("truncated"), std::string::npos);
}

TEST(FailureDetail, status_mismatch_detail_cuts_a_huge_body_at_four_kib_with_its_size) {
    const std::string body = "token=tok-123456 " + std::string(std::size_t{100} * 1024, 'y');
    const auto run = runScripted(
        kRetryProjectYaml, "item.read", [&](ScriptedHttpClient& http) { http.respond(400, body); });
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_TRUE(step->detail.ends_with("… (truncated, 100.0 KiB total)")) << step->detail.size();
    EXPECT_LT(step->detail.size(), ce::kDefaultFailureExcerptBytes + 100);
    EXPECT_GT(step->detail.size(), ce::kDefaultFailureExcerptBytes);
    EXPECT_EQ(step->detail.find("tok-123456"), std::string::npos);
}

TEST(FailureDetail, excerpt_size_option_takes_effect) {
    const std::string body = R"({"access_token":"tok-123456","error":")" +
                             std::string(std::size_t{8} * 1024, 'e') + R"("})";
    const auto detailWith = [&](const std::size_t bytes) {
        ce::RunOptions options;
        options.failureExcerptBytes = bytes;
        const auto run = runScripted(
            kRetryProjectYaml,
            "item.read",
            [&](ScriptedHttpClient& http) { http.respond(400, body); },
            {},
            options);
        EXPECT_TRUE(run.result.has_value());
        const auto* step = run.result ? finalStep(*run.result, "item.read") : nullptr;
        return step != nullptr ? step->detail : std::string{};
    };
    const std::string prefix = "HTTP 400 (expected 200)";
    const std::size_t quoteStart = prefix.size() + std::string_view{" — "}.size();

    const auto small = detailWith(64);
    EXPECT_EQ(small.find("… (truncated"), quoteStart + 64) << small;
    EXPECT_EQ(small.find("tok-123456"), std::string::npos) << small;

    const auto large = detailWith(16 * 1024);
    EXPECT_EQ(large.find("truncated"), std::string::npos);
    EXPECT_TRUE(large.ends_with("eee\"}")) << large.substr(large.size() - 16);
    EXPECT_EQ(large.find("tok-123456"), std::string::npos);

    EXPECT_EQ(detailWith(0), prefix);

    const std::string huge(std::size_t{1536} * 1024, 'h');
    ce::RunOptions options;
    options.failureExcerptBytes = std::size_t{4} * 1024 * 1024;
    const auto run = runScripted(
        kRetryProjectYaml,
        "item.read",
        [&](ScriptedHttpClient& http) { http.respond(400, huge); },
        {},
        options);
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->detail.find("… (truncated"), quoteStart + ce::kMaxFailureExcerptBytes);
}

TEST(FailureDetail, login_error_uses_the_run_excerpt_size) {
    constexpr const char* kActorYaml = R"YAML(
version: 1
name: LoginExcerpt
default_environment: local
environment:
  baseUrl: http://placeholder
actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /auth/login
      body: { email: "u@x.test" }
      expect_status: 200
      extract:
        token: $.token
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        actor: user
        expect_status: 200
)YAML";
    ce::RunOptions options;
    options.failureExcerptBytes = 32;
    const auto run = runScripted(
        kActorYaml,
        "item.read",
        [](ScriptedHttpClient& http) {
            http.respond(400, R"({"hint":"LOGIN-HINT",)" + std::string(500, 'l') + "\"}");
        },
        {},
        options);
    ASSERT_TRUE(run.result.has_value());
    const auto* step = finalStep(*run.result, "item.read");
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("LOGIN-HINT"), std::string::npos) << step->detail;
    EXPECT_NE(step->detail.find("… (truncated, "), std::string::npos) << step->detail;
    EXPECT_EQ(step->detail.find(std::string(40, 'l')), std::string::npos) << step->detail;
}

constexpr const char* kShowYaml = R"YAML(
version: 1
name: FailureShow
default_environment: local
failure_message:
  show: [$.error.message, $.errors, $.missing]
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
      own:
        method: GET
        path: /items/own
        expect_status: 200
        failure_message:
          show: [$.code]
      plain:
        method: GET
        path: /items/plain
        expect_status: 200
        failure_message:
          show: []
      token:
        method: GET
        path: /items/token
        expect_status: 200
        failure_message:
          show: [$.access_token, $.data]
)YAML";

[[nodiscard]] std::string showDetail(const std::string& op,
                                     const std::string& body,
                                     const ce::RunOptions& options = {}) {
    const auto run = runScripted(
        kShowYaml, op, [&](ScriptedHttpClient& http) { http.respond(400, body); }, {}, options);
    EXPECT_TRUE(run.result.has_value()) << (run.result ? "" : run.result.error().detail);
    const auto* step = run.result ? finalStep(*run.result, op) : nullptr;
    return step != nullptr ? step->detail : std::string{};
}

TEST(FailureDetail, project_show_list_selects_the_named_parts_in_order) {
    const auto detail = showDetail(
        "item.read",
        R"({"noise":"NOISE","errors":[{"field":"name"}],"error":{"message":"Name is required"}})");
    EXPECT_EQ(detail,
              R"(HTTP 400 (expected 200) — $.error.message: Name is required; )"
              R"($.errors: [{"field":"name"}])");
}

TEST(FailureDetail, operation_show_list_replaces_the_project_list) {
    const auto detail =
        showDetail("item.own", R"({"code":"E42","error":{"message":"PROJECT-LEVEL"}})");
    EXPECT_EQ(detail, "HTTP 400 (expected 200) — $.code: E42");
}

TEST(FailureDetail, empty_operation_show_list_uses_the_default_excerpt) {
    const std::string body = R"({"code":"E42","error":{"message":"m"}})";
    EXPECT_EQ(showDetail("item.plain", body), "HTTP 400 (expected 200) — " + body);
}

TEST(FailureDetail, show_list_that_matches_nothing_falls_back_to_the_default_excerpt) {
    const std::string body = R"({"unexpected":"shape"})";
    EXPECT_EQ(showDetail("item.read", body), "HTTP 400 (expected 200) — " + body);
}

TEST(FailureDetail, show_list_is_ignored_for_a_body_that_is_not_json) {
    EXPECT_EQ(showDetail("item.read", "Bad Gateway: upstream down"),
              "HTTP 400 (expected 200) — Bad Gateway: upstream down");
}

TEST(FailureDetail, shown_object_is_compact_json_with_token_fields_masked) {
    const auto detail = showDetail(
        "item.token",
        R"({"access_token":"tok-123456","data":{"user":"u","refresh_token":"rrr-999999"}})");
    EXPECT_EQ(detail.find("tok-123456"), std::string::npos) << detail;
    EXPECT_EQ(detail.find("rrr-999999"), std::string::npos) << detail;
    EXPECT_NE(detail.find("$.access_token: <redacted>"), std::string::npos) << detail;
    EXPECT_NE(detail.find(R"($.data: {"refresh_token":"<redacted>","user":"u"})"),
              std::string::npos)
        << detail;
}

TEST(FailureDetail, size_limit_applies_to_the_combined_selected_text) {
    ce::RunOptions options;
    options.failureExcerptBytes = 40;
    const std::string body = R"({"errors":[")" + std::string(100, 'e') +
                             R"("],"error":{"message":")" + std::string(100, 'm') + R"("}})";
    const auto detail = showDetail("item.read", body, options);
    const std::string head = "HTTP 400 (expected 200) — ";
    ASSERT_TRUE(detail.starts_with(head + "$.error.message: mmm")) << detail;
    EXPECT_EQ(detail.find("… (truncated, "), head.size() + 40) << detail;

    options.failureExcerptBytes = 0;
    EXPECT_EQ(showDetail("item.read", body, options), "HTTP 400 (expected 200)");
}

constexpr const char* kSigV4UploadYaml = R"YAML(
version: 1
name: SigV4Upload
default_environment: local
environment:
  baseUrl: http://placeholder
actors:
  aws:
    auth:
      strategy: aws_sigv4
      access_key: AKIDEXAMPLE
      secret_key: wJalrXUtnFEMIK7MDENG
      region: us-east-1
      service: execute-api
resources:
  upload:
    operations:
      inline:
        method: POST
        path: /upload
        auth:
          type: aws_sigv4
          access_key: AKIDEXAMPLE
          secret_key: wJalrXUtnFEMIK7MDENG
          region: us-east-1
          service: execute-api
        headers:
          Content-Type: multipart/form-data
        body_form:
          note: hello
        expect_status: 200
      actor:
        method: POST
        path: /upload
        actor: aws
        headers:
          Content-Type: multipart/form-data
        body_form:
          note: hello
        expect_status: 200
)YAML";

TEST(SigV4Upload, multipart_refusal_names_the_service_in_the_step_detail) {
    for (const std::string op : {"upload.inline", "upload.actor"}) {
        const auto run =
            runScripted(kSigV4UploadYaml, op, [](ScriptedHttpClient& http) { http.respond(200); });
        ASSERT_TRUE(run.result.has_value()) << op << ": " << run.result.error().detail;
        EXPECT_EQ(run.sends, 0U) << op;
        const auto* step = finalStep(*run.result, op);
        ASSERT_NE(step, nullptr) << op;
        EXPECT_NE(step->detail.find("AWS SigV4 cannot sign a multipart upload for service "
                                    "'execute-api'; only s3 is supported"),
                  std::string::npos)
            << op << ": " << step->detail;
        EXPECT_EQ(step->detail.find("access_key"), std::string::npos) << op << ": " << step->detail;
    }
}

TEST(UnknownActor, step_naming_undefined_actor_fails_before_sending) {
    // Load-time validation may reject this first; either way the request
    // must never go out unauthenticated and the error must name the actor.
    const auto run = runScripted(
        kSimpleProjectYaml,
        "ping.get",
        [](ScriptedHttpClient& http) { http.respond(200); },
        [](ce::Project& project) {
            project.resources.at(ce::ResourceId{"ping"}).operations.at("get").actor =
                ce::ActorId{"ghost"};
        });
    EXPECT_EQ(run.sends, 0u) << "request was sent without the actor's credentials";
    if (run.result.has_value()) {
        const auto* step = finalStep(*run.result, "ping.get");
        ASSERT_NE(step, nullptr);
        EXPECT_EQ(step->status, ce::StepResult::Status::Failed);
        EXPECT_EQ(step->error, ce::ErrorCode::RefUndefined);
        EXPECT_NE(step->detail.find("ghost"), std::string::npos) << step->detail;
    } else {
        EXPECT_EQ(run.result.error().code, ce::ErrorCode::RefUndefined);
        EXPECT_NE(run.result.error().detail.find("ghost"), std::string::npos);
    }
}

TEST(UnknownActor, poll_until_naming_undefined_actor_never_polls_unauthenticated) {
    constexpr const char* kPollYaml = R"YAML(
version: 1
name: PollActor
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  job:
    operations:
      start:
        method: GET
        path: /jobs/1
        expect_status: 202
        poll_until:
          method: GET
          path: /jobs/1/status
          success_when: "$.status == 'DONE'"
          interval: 50ms
          timeout: 5s
          max_attempts: 3
)YAML";
    const auto run = runScripted(
        kPollYaml,
        "job.start",
        [](ScriptedHttpClient& http) {
            http.respond(202);
            http.respond(200, R"({"status":"DONE"})");
        },
        [](ce::Project& project) {
            auto& op = project.resources.at(ce::ResourceId{"job"}).operations.at("start");
            op.pollUntil->actor = ce::ActorId{"ghost"};
        });
    EXPECT_LE(run.sends, 1u) << "poll request was sent without the actor's credentials";
    if (run.result.has_value()) {
        const auto* step = finalStep(*run.result, "job.start");
        ASSERT_NE(step, nullptr);
        EXPECT_EQ(step->status, ce::StepResult::Status::Failed);
        EXPECT_EQ(step->error, ce::ErrorCode::RefUndefined);
        EXPECT_NE(step->detail.find("ghost"), std::string::npos) << step->detail;
    } else {
        EXPECT_EQ(run.result.error().code, ce::ErrorCode::RefUndefined);
    }
}
