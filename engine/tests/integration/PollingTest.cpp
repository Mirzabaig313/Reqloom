// PollingTest — end-to-end coverage for poll_until.
//
// Uses the mock SUT's "sequence" route capability to return a series of
// responses on the polling endpoint:
//   1st call → PENDING
//   2nd call → PROCESSING
//   3rd call → COMPLETED   ← success_when matches; extractions run
//
// A second test exercises fail_when by configuring a sequence whose
// second response has status:"FAILED".
//
// Each test fails on the pre-3c commit (poll_until was a parsed-but-
// ignored field; the executor never entered the polling phase, so
// success_when was never evaluated and the operation either succeeded
// on the wrong response or failed an extraction).
#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <chrono>
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

class PollingScratchProject {
public:
    explicit PollingScratchProject(const std::string& yamlBody) {
        const auto unique = "chainapi-polling-itest-" + std::to_string(::getpid()) + "-" +
                            std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
        std::ofstream{path_ / "chainapi.yaml"} << yamlBody;
    }

    ~PollingScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] fs::path yaml() const { return path_ / "chainapi.yaml"; }

private:
    fs::path path_;
    inline static int counter_{0};
};

}  // namespace

class PollingFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "polling-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(PollingFixture, success_when_matches_after_a_few_polls) {
    PollingScratchProject project(R"YAML(
version: 1
name: PollingSample
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
  payment:
    operations:
      pay:
        method: POST
        path: /api/v1/payments
        actor: user
        body: { method: "card" }
        expect_status: [200, 202]
        poll_until:
          method: GET
          path: /api/v1/payments/pay-42/status
          success_when: "$.status == 'COMPLETED'"
          fail_when:    "$.status in ['FAILED', 'CANCELLED']"
          interval: 50ms
          timeout: 5s
          max_attempts: 20
        extract:
          payment_id: $.id
          settled_at: $.settled_at
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"payment.pay"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded()) << "outcome was Failed/Cancelled";

    // The final-poll response carried `settled_at` — its presence in
    // the run context proves extractions ran against the COMPLETED
    // response, not the initial 202 launch ack (which had no settled_at).
    const auto& payments = ctx.instances(ce::ResourceId{"payment"});
    ASSERT_FALSE(payments.empty());
    const auto& vars = payments.back().variables;
    EXPECT_EQ(vars.at("payment_id"), "pay-42");
    EXPECT_EQ(vars.at("settled_at"), "2026-05-24T12:00:00Z");
}

TEST_F(PollingFixture, fail_when_short_circuits_with_PollFailPredicate) {
    PollingScratchProject project(R"YAML(
version: 1
name: PollingFailSample
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
  job:
    operations:
      submit:
        method: POST
        path: /api/v1/jobs
        actor: user
        body: { kind: "rebuild" }
        expect_status: [200, 202]
        poll_until:
          method: GET
          path: /api/v1/jobs/job-99/status
          success_when: "$.status == 'COMPLETED'"
          fail_when:    "$.status == 'FAILED'"
          interval: 50ms
          timeout: 5s
          max_attempts: 20
        extract:
          job_id: $.data.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"job.submit"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded()) << "outcome should be Failed";

    // Find the submit step and verify the failure code is the dedicated
    // poll-fail-predicate code, not a generic HTTP/network error.
    const ce::StepResult* submit = nullptr;
    for (const auto& s : result->steps) {
        if (s.op.value == "job.submit") submit = &s;
    }
    ASSERT_NE(submit, nullptr);
    EXPECT_EQ(submit->status, ce::StepResult::Status::Failed);
    ASSERT_TRUE(submit->error.has_value());
    EXPECT_EQ(*submit->error, ce::ErrorCode::PollFailPredicate);
}

TEST_F(PollingFixture, zero_interval_does_not_busy_loop_and_includes_last_status) {
    // H2 + L1 regression. A misconfigured `interval: 0ms` (no backoff)
    // would have busy-looped the SUT until max_attempts in pre-fix
    // versions — burning CPU and finishing in microseconds. The poll
    // loop now floors the inter-attempt delay at 50ms.
    //
    // Also pins the L1 enrichment: the failure detail surfaces the
    // last response's HTTP status so the user can see what the server
    // was saying when the loop gave up.
    PollingScratchProject project(R"YAML(
version: 1
name: BusyLoopGuard
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
  stuck:
    operations:
      run:
        method: POST
        path: /api/v1/never-completes
        actor: user
        expect_status: [200, 202]
        poll_until:
          method: GET
          path: /api/v1/never-completes/stuck-1/status
          success_when: "$.status == 'COMPLETED'"
          interval: 0ms
          timeout: 500ms
          max_attempts: 1000
        extract:
          stuck_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    const auto t0 = std::chrono::steady_clock::now();
    auto result = engine.run(*loaded, ce::OperationId{"stuck.run"}, ctx);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());

    const ce::StepResult* step = nullptr;
    for (const auto& s : result->steps) {
        if (s.op.value == "stuck.run") step = &s;
    }
    ASSERT_NE(step, nullptr);
    ASSERT_EQ(step->status, ce::StepResult::Status::Failed);
    ASSERT_TRUE(step->error.has_value());
    EXPECT_EQ(*step->error, ce::ErrorCode::PollTimeout);

    // L1: timeout detail must reference the last-response status.
    EXPECT_NE(step->detail.find("HTTP 200"), std::string::npos)
        << "expected last-response context in detail; got: " << step->detail;

    // H2: the timeout (500ms) must actually elapse — if the loop were
    // busy-looping we'd terminate in < 50ms via max_attempts (1000).
    // With the 50ms floor, the loop hits the wall-clock deadline, not
    // the attempt budget, so total elapsed must be ≥ ~timeout.
    EXPECT_GE(elapsed, std::chrono::milliseconds{400})
        << "polling exited too early — H2 floor probably not applied";
}

TEST_F(PollingFixture, api_key_actor_runs_an_operation_end_to_end) {
    // Slice 4c integration check. The api_key strategy makes no HTTP
    // call itself; this test proves the engine still treats an
    // api_key-only actor as "authenticated" and routes the operation
    // through to the SUT. If the auth bookkeeping is wrong, the
    // op fails before the request reaches the wire.
    PollingScratchProject project(R"YAML(
version: 1
name: ApiKeyEndToEnd
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  service:
    auth:
      strategy: api_key
      key: "sk_live_abc"
      location: header
      name: X-API-Key

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-api-key
        actor: service
        expect_status: 200
        extract:
          ping_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    // The session should carry both the variable and the auto-injected
    // header. Manual cache inspection.
    const auto* session = ctx.session(ce::ActorId{"service"});
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->variables.at("key"), "sk_live_abc");
    EXPECT_EQ(session->injectHeaders.at("X-API-Key"), "sk_live_abc");

    // The operation extracted ping_id from the SUT's response, proving
    // the request actually reached it.
    const auto& pings = ctx.instances(ce::ResourceId{"ping"});
    ASSERT_FALSE(pings.empty());
    EXPECT_EQ(pings.back().variables.at("ping_id"), "apikey-1");
}

TEST_F(PollingFixture, oauth2_client_credentials_actor_runs_end_to_end) {
    // Slice 4d integration check. The strategy POSTs to /oauth/token,
    // extracts access_token, and auto-injects Authorization: Bearer.
    // This proves the whole pipeline (parse → strategy → token POST →
    // session populate → executor merge → SUT) works against the
    // real HTTP client.
    PollingScratchProject project(R"YAML(
version: 1
name: OAuth2EndToEnd
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  service:
    auth:
      strategy: oauth2_client_credentials
      token_url: "{{env.baseUrl}}/oauth/token"
      client_id: "client-x"
      client_secret: "secret-y"
      scope: "read:ping"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-bearer
        actor: service
        expect_status: 200
        extract:
          ping_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    // Strategy stored the token endpoint's response in the session +
    // auto-populated the Bearer header.
    const auto* session = ctx.session(ce::ActorId{"service"});
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->variables.at("access_token"), "oauth-tok-7");
    EXPECT_EQ(session->variables.at("token_type"), "Bearer");
    EXPECT_EQ(session->injectHeaders.at("Authorization"), "Bearer oauth-tok-7");

    // The /api/v1/with-bearer route was reached and the response
    // extracted — proves the Bearer header carried through from the
    // session into the outgoing operation.
    const auto& pings = ctx.instances(ce::ResourceId{"ping"});
    ASSERT_FALSE(pings.empty());
    EXPECT_EQ(pings.back().variables.at("ping_id"), "bearer-1");
}

TEST_F(PollingFixture, oauth2_password_actor_runs_end_to_end) {
    // Slice 4e integration check. The strategy POSTs to /oauth/token
    // with grant_type=password (plus username/password + client_*),
    // extracts the access_token, and auto-injects Authorization: Bearer.
    // Reuses the same /oauth/token mock route as the client_credentials
    // test — both grants speak the same wire shape so the SUT doesn't
    // need to distinguish them.
    PollingScratchProject project(R"YAML(
version: 1
name: OAuth2PasswordEndToEnd
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  user:
    auth:
      strategy: oauth2_password
      token_url: "{{env.baseUrl}}/oauth/token"
      client_id: "client-x"
      client_secret: "secret-y"
      username: "alice@example.com"
      password: "wonderland"
      scope: "read:ping"

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
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    const auto* session = ctx.session(ce::ActorId{"user"});
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->variables.at("access_token"), "oauth-tok-7");
    EXPECT_EQ(session->injectHeaders.at("Authorization"), "Bearer oauth-tok-7");

    const auto& pings = ctx.instances(ce::ResourceId{"ping"});
    ASSERT_FALSE(pings.empty());
    EXPECT_EQ(pings.back().variables.at("ping_id"), "bearer-1");
}

TEST_F(PollingFixture, oauth1_actor_runs_an_operation_end_to_end) {
    // Slice 4f integration check. OAuth1 signs per-request, so the
    // proof is: parse → authenticate → executor calls signOAuth1Request
    // → mock SUT receives the request and returns 200. The SUT can't
    // verify the OAuth1 signature itself (would need its own crypto),
    // but the strategy populating the session and the signer running
    // without errors is what we're testing here. Unit tests
    // (OAuth1Signer.matches_rfc5849_section_3_4_3_reference_vector)
    // pin the signature itself against the RFC.
    PollingScratchProject project(R"YAML(
version: 1
name: OAuth1EndToEnd
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  app:
    auth:
      strategy: oauth1
      consumer_key: "ck"
      consumer_secret: "cs"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-bearer
        actor: app
        expect_status: 200
        extract:
          ping_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    // The session must carry the credentials and the signing flag.
    const auto* session = ctx.session(ce::ActorId{"app"});
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->variables.at("consumer_key"), "ck");
    EXPECT_EQ(session->variables.at("consumer_secret"), "cs");
    EXPECT_EQ(session->signingScheme, ce::ActorSession::SigningScheme::OAuth1HmacSha1);

    // The /api/v1/with-bearer route was reached and the response
    // extracted — proves the signed request actually went out.
    const auto& pings = ctx.instances(ce::ResourceId{"ping"});
    ASSERT_FALSE(pings.empty());
    EXPECT_EQ(pings.back().variables.at("ping_id"), "bearer-1");
}

// ─── Slice 3g — per-poll-attempt timeline visibility ────────────────────────
//
// Each poll attempt is recorded as a step in the run timeline.
// Before this slice, only the parent step row was recorded; the timeline
// hid every poll request behind a single elapsed-time bar. These tests
// fail on the parent commit (zero per-attempt rows in `result->steps`).

TEST_F(PollingFixture, success_records_one_step_per_poll_attempt) {
    // The payment-status sequence in polling-routes.json returns
    // PENDING → PROCESSING → COMPLETED, so we expect exactly three
    // per-attempt rows plus the parent step.
    PollingScratchProject project(R"YAML(
version: 1
name: PollAttemptTimeline
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
  payment:
    operations:
      pay:
        method: POST
        path: /api/v1/payments
        actor: user
        body: { method: "card" }
        expect_status: [200, 202]
        poll_until:
          method: GET
          path: /api/v1/payments/pay-42/status
          success_when: "$.status == 'COMPLETED'"
          fail_when:    "$.status in ['FAILED', 'CANCELLED']"
          interval: 50ms
          timeout: 5s
          max_attempts: 20
        extract:
          payment_id: $.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"payment.pay"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    std::vector<const ce::StepResult*> attempts;
    const ce::StepResult* parent = nullptr;
    for (const auto& s : result->steps) {
        if (s.op.value != "payment.pay") continue;
        if (s.pollAttempt)
            attempts.push_back(&s);
        else
            parent = &s;
    }

    ASSERT_NE(parent, nullptr);
    ASSERT_EQ(attempts.size(), 3u);

    // Order of attempts is the order they were recorded — sequence-route
    // returns PENDING, PROCESSING, COMPLETED in that order.
    EXPECT_EQ(attempts[0]->pollAttempt.value(), 1);
    EXPECT_EQ(attempts[0]->status, ce::StepResult::Status::Pending);
    EXPECT_NE(attempts[0]->detail.find("in_progress"), std::string::npos);

    EXPECT_EQ(attempts[1]->pollAttempt.value(), 2);
    EXPECT_EQ(attempts[1]->status, ce::StepResult::Status::Pending);
    EXPECT_NE(attempts[1]->detail.find("in_progress"), std::string::npos);

    EXPECT_EQ(attempts[2]->pollAttempt.value(), 3);
    EXPECT_EQ(attempts[2]->status, ce::StepResult::Status::Succeeded);
    EXPECT_NE(attempts[2]->detail.find("success_when"), std::string::npos);

    // Parent step still landed and its summary is independent of the
    // per-attempt rows.
    EXPECT_EQ(parent->status, ce::StepResult::Status::Succeeded);
    EXPECT_FALSE(parent->pollAttempt.has_value());
}

TEST_F(PollingFixture, fail_predicate_records_attempt_with_PollFailPredicate) {
    // The job-status sequence is QUEUED → FAILED, so attempt 1 is
    // in-progress and attempt 2 trips fail_when.
    PollingScratchProject project(R"YAML(
version: 1
name: PollFailTimeline
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
  job:
    operations:
      submit:
        method: POST
        path: /api/v1/jobs
        actor: user
        body: { kind: "rebuild" }
        expect_status: [200, 202]
        poll_until:
          method: GET
          path: /api/v1/jobs/job-99/status
          success_when: "$.status == 'COMPLETED'"
          fail_when:    "$.status == 'FAILED'"
          interval: 50ms
          timeout: 5s
          max_attempts: 20
        extract:
          job_id: $.data.id
)YAML");

    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(*loaded, ce::OperationId{"job.submit"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());

    const ce::StepResult* failedAttempt = nullptr;
    int attemptCount = 0;
    for (const auto& s : result->steps) {
        if (s.op.value != "job.submit" || !s.pollAttempt) continue;
        ++attemptCount;
        if (s.error && *s.error == ce::ErrorCode::PollFailPredicate) {
            failedAttempt = &s;
        }
    }

    EXPECT_GE(attemptCount, 1) << "expected at least one per-attempt row";
    ASSERT_NE(failedAttempt, nullptr) << "no attempt row carried PollFailPredicate";
    EXPECT_EQ(failedAttempt->status, ce::StepResult::Status::Failed);
    EXPECT_NE(failedAttempt->detail.find("fail_when"), std::string::npos);
}
