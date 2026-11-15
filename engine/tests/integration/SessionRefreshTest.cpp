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

#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <support/TempPath.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "refresh-project" / "reqloom.yaml");
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

// ─── D-03: refreshed credentials replace the session's injected headers ─────
//
// Strategies like bearer / oauth2 build `Authorization` into the session's
// pre-resolved injectHeaders at login. A refresh that only merged variables
// left that header stale, so every request after a refresh still carried
// the old token. The fixture above misses this because it injects through
// a templated `inject.headers`, which is re-resolved on every request.

namespace {

/// Answers by path: token / refresh endpoints return the next token in
/// sequence; everything else returns 200. Records every request.
class RoutedHttpClient final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests_.push_back(request);
        ce::HttpResponse response;
        response.status = 200;
        if (request.url.ends_with("/oauth/token")) {
            response.body = R"({"access_token":"tok-initial","expires_in":3600})";
        } else if (request.url.ends_with("/auth/refresh")) {
            response.body = R"({"access_token":"tok-refreshed","token":"tok-refreshed"})";
        } else {
            response.body = "{}";
        }
        return response;
    }

    [[nodiscard]] std::string lastAuthorizationFor(std::string_view suffix) const {
        for (auto it = requests_.rbegin(); it != requests_.rend(); ++it) {
            if (it->url.ends_with(suffix)) {
                const auto header = it->headers.find("Authorization");
                return header == it->headers.end() ? std::string{} : header->second;
            }
        }
        return {};
    }

private:
    std::vector<ce::HttpRequest> requests_;
};

void expectRefreshReplacesInjectedHeader(const std::string& actorAuthYaml,
                                         const std::string& refreshExtractYaml) {
    const auto dir = ct::uniqueTempPath("reqloom-refresh-inject");
    fs::create_directories(dir);
    std::ofstream{dir / "reqloom.yaml"} << R"YAML(
version: 1
name: RefreshInject
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  svc:
    auth:
)YAML" + actorAuthYaml + R"YAML(
    session:
      ttl: 0s
      refresh:
        method: POST
        path: /auth/refresh
        extract:
)YAML" + refreshExtractYaml + R"YAML(

resources:
  me:
    operations:
      get:
        method: GET
        path: /me
        actor: svc
        expect_status: 200
)YAML";
    auto project = ce::parseProject(dir / "reqloom.yaml");
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(project.has_value()) << project.error().detail;

    auto http = std::make_unique<RoutedHttpClient>();
    auto* routed = http.get();
    ce::ExecutionEngine engine{
        ce::ExecutionEngine::Dependencies{std::move(http), nullptr, nullptr, nullptr, nullptr}};

    std::size_t refreshes{};
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (std::holds_alternative<ce::SessionRefreshed>(ev)) {
            ++refreshes;
        }
    });

    ce::RunContext ctx;
    auto first = engine.run(*project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(first.has_value() && first->succeeded());
    // ttl 0s: the second run refreshes instead of re-authenticating.
    auto second = engine.run(*project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(second.has_value() && second->succeeded());
    ASSERT_EQ(refreshes, 1u);

    EXPECT_EQ(routed->lastAuthorizationFor("/me"), "Bearer tok-refreshed")
        << "request after refresh still carried the pre-refresh credential";
}

}  // namespace

TEST(SessionRefreshInject, oauth2_refresh_replaces_stale_authorization_header) {
    expectRefreshReplacesInjectedHeader(R"YAML(      strategy: oauth2_client_credentials
      token_url: "{{env.baseUrl}}/oauth/token"
      client_id: "client-x"
      client_secret: "secret-y")YAML",
                                        "          access_token: $.access_token");
}

TEST(SessionRefreshInject, bearer_refresh_replaces_stale_authorization_header) {
    expectRefreshReplacesInjectedHeader(R"YAML(      strategy: bearer
      token: "tok-static")YAML",
                                        "          token: $.token");
}
