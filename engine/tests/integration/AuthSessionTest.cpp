// AuthSessionTest — actor session lifetime, auth failure causes on the step
// result, and the inline OAuth2 token cache key. Uses a scripted HttpClient.

#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;

namespace {

/// Token endpoints answer with `tokenStatus` / `tokenBody`; a URL on host
/// `unreachable.test` fails with NetworkDns; everything else returns 200 `{}`.
class ScriptedHttpClient final : public ce::HttpClient {
public:
    int tokenStatus{200};
    std::string tokenBody{R"({"access_token":"tok-1","expires_in":3600})"};
    std::vector<ce::HttpRequest> requests;

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests.push_back(request);
        if (request.url.find("unreachable.test") != std::string::npos) {
            return std::unexpected(ce::ReqloomError{ce::ErrorCode::NetworkDns,
                                                    ce::ErrorClass::Network,
                                                    "could not resolve host unreachable.test"});
        }
        ce::HttpResponse response;
        response.status = 200;
        response.body = "{}";
        if (request.url.find("/token") != std::string::npos) {
            response.status = tokenStatus;
            response.body = tokenBody;
        }
        return response;
    }

    [[nodiscard]] std::size_t countUrl(const std::string& url) const {
        std::size_t n = 0;
        for (const auto& r : requests) {
            n += r.url == url ? 1U : 0U;
        }
        return n;
    }
};

ce::Project makeActorProject(const std::string& tokenUrl) {
    ce::Project p;
    p.name = "auth-session";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://api.test"}};

    ce::Actor actor;
    actor.id = ce::ActorId{"svc"};
    actor.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    actor.authConfig["token_url"] = tokenUrl;
    actor.authConfig["client_id"] = "client-1";
    actor.authConfig["client_secret"] = "client-secret-1";
    // The token endpoint is on another host than baseUrl: the actor must list both (FR-15.9).
    actor.allowedHosts = {"api.test", "idp.test"};
    p.actors[actor.id] = actor;

    ce::Operation op;
    op.id = ce::OperationId{"me.get"};
    op.resource = ce::ResourceId{"me"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/me";
    op.expectStatus = 200;
    op.actor = actor.id;
    ce::Resource me;
    me.id = ce::ResourceId{"me"};
    me.operations["get"] = op;
    p.resources[me.id] = std::move(me);
    return p;
}

struct Harness {
    ScriptedHttpClient* http{nullptr};
    std::unique_ptr<ce::ExecutionEngine> engine;
};

Harness makeHarness() {
    auto http = std::make_unique<ScriptedHttpClient>();
    Harness h;
    h.http = http.get();
    h.engine = std::make_unique<ce::ExecutionEngine>(reqloom::tests::withApprovedHosts(
        ce::ExecutionEngine::Dependencies{std::move(http), nullptr, nullptr, nullptr, nullptr}));
    return h;
}

const ce::StepResult& onlyStep(const ce::RunResult& result) {
    EXPECT_EQ(result.steps.size(), 1U);
    return result.steps.front();
}

}  // namespace

TEST(AuthSession, oauth2_actor_session_ends_when_the_token_expires) {
    auto h = makeHarness();
    // 31 s minus the 30 s safety margin: the session is live for 1 s.
    h.http->tokenBody = R"({"access_token":"tok-short","expires_in":31})";
    const auto project = makeActorProject("http://idp.test/token");

    ce::RunContext ctx;
    auto first = h.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(first.has_value() && first->succeeded());
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});
    auto second = h.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(second.has_value() && second->succeeded());

    EXPECT_EQ(h.http->countUrl("http://idp.test/token"), 2U)
        << "the actor kept presenting a token past its expires_in";
}

TEST(AuthSession, oauth2_actor_session_without_expires_in_keeps_the_ttl) {
    auto h = makeHarness();
    h.http->tokenBody = R"({"access_token":"tok-long"})";
    const auto project = makeActorProject("http://idp.test/token");

    ce::RunContext ctx;
    ASSERT_TRUE(h.engine->run(project, ce::OperationId{"me.get"}, ctx));
    ASSERT_TRUE(h.engine->run(project, ce::OperationId{"me.get"}, ctx));
    EXPECT_EQ(h.http->countUrl("http://idp.test/token"), 1U);
}

TEST(AuthSession, rejected_login_reaches_the_step_detail) {
    auto h = makeHarness();
    h.http->tokenStatus = 401;
    h.http->tokenBody = R"({"error":"invalid_client","access_token":"tok-not-for-display"})";
    const auto project = makeActorProject("http://idp.test/token");

    ce::RunContext ctx;
    auto result = h.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    const auto& step = onlyStep(*result);
    EXPECT_EQ(step.status, ce::StepResult::Status::Failed);
    EXPECT_EQ(step.error, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(step.detail.find("svc"), std::string::npos) << step.detail;
    EXPECT_NE(step.detail.find("HTTP 401"), std::string::npos) << step.detail;
    EXPECT_NE(step.detail.find("invalid_client"), std::string::npos) << step.detail;
    EXPECT_EQ(step.detail.find("tok-not-for-display"), std::string::npos) << step.detail;
    EXPECT_EQ(step.detail.find("client-secret-1"), std::string::npos) << step.detail;
}

TEST(AuthSession, unreachable_token_endpoint_reaches_the_step_detail) {
    auto h = makeHarness();
    auto project = makeActorProject("http://unreachable.test/token");
    project.actors.at(ce::ActorId{"svc"}).allowedHosts.emplace_back("unreachable.test");

    ce::RunContext ctx;
    auto result = h.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    const auto& step = onlyStep(*result);
    EXPECT_EQ(step.error, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(step.detail.find("could not resolve host"), std::string::npos) << step.detail;
}

TEST(AuthSession, missing_credential_reaches_the_step_detail) {
    auto h = makeHarness();
    auto project = makeActorProject("http://idp.test/token");
    project.actors.at(ce::ActorId{"svc"}).authConfig.erase("client_id");

    ce::RunContext ctx;
    auto result = h.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    const auto& step = onlyStep(*result);
    EXPECT_EQ(step.error, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(step.detail.find("client_id"), std::string::npos) << step.detail;
}

TEST(AuthSession, inline_oauth2_cache_is_keyed_by_resolved_values) {
    auto h = makeHarness();
    ce::Project p;
    p.name = "inline-oauth2";
    p.defaultEnvironment = "a";
    p.environments["a"] = {{"baseUrl", "http://api.test"},
                           {"token_url", "http://idp-a.test/token"}};
    p.environments["b"] = {{"baseUrl", "http://api.test"},
                           {"token_url", "http://idp-b.test/token"}};
    ce::Operation op;
    op.id = ce::OperationId{"me.get"};
    op.resource = ce::ResourceId{"me"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/me";
    op.expectStatus = 200;
    op.inlineAuth = ce::InlineAuth{.type = ce::InlineAuthType::OAuth2,
                                   .oauth2TokenUrl = "{{env.token_url}}",
                                   .oauth2ClientId = "cid",
                                   .oauth2ClientSecret = "csecret-1"};
    ce::Resource me;
    me.id = ce::ResourceId{"me"};
    me.operations["get"] = op;
    p.resources[me.id] = std::move(me);

    ce::RunContext ctx;
    ce::RunOptions inA;
    inA.environment = "a";
    ce::RunOptions inB;
    inB.environment = "b";
    ASSERT_TRUE(h.engine->run(p, ce::OperationId{"me.get"}, ctx, inA));
    ASSERT_TRUE(h.engine->run(p, ce::OperationId{"me.get"}, ctx, inB));
    ASSERT_TRUE(h.engine->run(p, ce::OperationId{"me.get"}, ctx, inA));

    EXPECT_EQ(h.http->countUrl("http://idp-a.test/token"), 1U);
    EXPECT_EQ(h.http->countUrl("http://idp-b.test/token"), 1U)
        << "a token fetched from one endpoint was reused for another";
}
