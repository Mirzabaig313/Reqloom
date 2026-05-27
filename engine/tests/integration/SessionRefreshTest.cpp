// SessionRefreshTest — confirms two flavors of session lifecycle work
// end-to-end against the mock SUT:
//
//   1. TTL expiry triggers the actor's `session.refresh:` block
//      instead of a full re-authentication. The mock SUT is configured
//      to serve a different access token on /api/v1/auth/refresh than
//      it does on /api/v1/auth/login, so the test can prove the engine
//      took the cheaper path.
//
//   2. A 401 from the API mid-step triggers a one-shot re-auth and
//      retry of the original operation. Without that path, the step
//      would surface E_HTTP_4XX with the user's expected status of
//      200 unsatisfied.
//
// Both tests share one fixture project; the schema is small enough to
// cover both surfaces without confusing the contracts.

#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "refresh-project" / "chainapi.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    project->environments["local"]["baseUrl"] = mockBaseUrl;
    return std::move(*project);
}

[[nodiscard]] nlohmann::json fetchLastRequest(const std::string& baseUrl, const std::string& path) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), &curl_easy_cleanup};
    EXPECT_NE(curl.get(), nullptr);

    const auto url = baseUrl + "/__mock/last-request?path=" + path;
    std::string body;
    auto writer = +[](char* ptr, std::size_t size, std::size_t nmemb, void* ud) -> std::size_t {
        auto* out = static_cast<std::string*>(ud);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writer);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    auto rc = curl_easy_perform(curl.get());
    EXPECT_EQ(rc, CURLE_OK) << curl_easy_strerror(rc);
    return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

class SessionRefreshFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "refresh-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(SessionRefreshFixture, ttl_expiry_uses_refresh_block_instead_of_full_re_auth) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::SessionRefreshed> refreshEvents;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::SessionRefreshed>(&ev)) {
            refreshEvents.push_back(*e);
        }
    });

    ce::RunContext ctx;

    // First run: full login, populates the session. With ttl=0s the
    // session is "live" but immediately stale on the next ensureSession
    // call.
    auto first = engine.run(project, ce::OperationId{"me.get_first"}, ctx);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().detail);
    ASSERT_TRUE(first->succeeded());
    EXPECT_TRUE(refreshEvents.empty()) << "first run should not emit SessionRefreshed";

    // Second run: ttl already expired, but the refresh block exists.
    // The engine must call /api/v1/auth/refresh (not /login) and merge
    // the new token into the existing session.
    auto second = engine.run(project, ce::OperationId{"me.get_after_expiry"}, ctx);
    ASSERT_TRUE(second.has_value()) << (second ? "" : second.error().detail);
    ASSERT_TRUE(second->succeeded());

    ASSERT_EQ(refreshEvents.size(), 1u)
        << "exactly one SessionRefreshed event should fire on TTL expiry";
    EXPECT_EQ(refreshEvents[0].trigger, ce::SessionRefreshed::Trigger::Expiry);
    EXPECT_EQ(refreshEvents[0].actor.value, "user");

    // Refresh endpoint received the existing refresh_token.
    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/auth/refresh");
    ASSERT_TRUE(cap["found"].get<bool>())
        << "refresh endpoint was not hit — engine did a full re-auth instead";
    EXPECT_EQ(cap["method"].get<std::string>(), "POST");
    EXPECT_NE(cap["raw_body"].get<std::string>().find("rt-1"), std::string::npos)
        << "refresh body did not carry {{user.refresh_token}} — was: "
        << cap["raw_body"].get<std::string>();

    // Confirm the second /me request used the refreshed token, not the
    // initial one. The injected Authorization header reflects the new
    // session variable.
    auto meCap = fetchLastRequest(harness_->baseUrl(), "/api/v1/me");
    ASSERT_TRUE(meCap["found"].get<bool>());
    EXPECT_EQ(meCap["headers"]["authorization"].get<std::string>(), "Bearer tok-refreshed");
}

TEST_F(SessionRefreshFixture, http_401_triggers_one_shot_reauth_and_retry) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::SessionRefreshed> refreshEvents;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::SessionRefreshed>(&ev)) {
            refreshEvents.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"protected.reads_protected"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded()) << "401 recovery did not produce a successful retry";

    ASSERT_EQ(refreshEvents.size(), 1u);
    EXPECT_EQ(refreshEvents[0].trigger, ce::SessionRefreshed::Trigger::Unauthorized);
    EXPECT_EQ(refreshEvents[0].actor.value, "user");

    // The mock SUT's sequence served 401 first, then 200 with body
    // {"data":{"id":"ok-after-reauth"}}. If the extraction trace shows
    // that id, the retry consumed the second response (proof of
    // recovery, end-to-end).
    bool sawRecoveredId = false;
    for (const auto& trace : ctx.extractionTrace()) {
        if (trace.op.value == "protected.reads_protected" && trace.variableName == "id") {
            EXPECT_EQ(trace.outcome, ce::ExtractionTrace::Outcome::Resolved);
            EXPECT_EQ(trace.value, "ok-after-reauth");
            sawRecoveredId = true;
        }
    }
    EXPECT_TRUE(sawRecoveredId);

    // The step row's `attempts` count should reflect the recovery —
    // first attempt + retry = 2.
    bool sawTwoAttempts = false;
    for (const auto& step : result->steps) {
        if (step.op.value == "protected.reads_protected" && !step.pollAttempt) {
            EXPECT_EQ(step.attempts, 2);
            sawTwoAttempts = true;
        }
    }
    EXPECT_TRUE(sawTwoAttempts);
}
