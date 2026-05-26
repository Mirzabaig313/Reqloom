// Unit tests for the Authenticator interface and selectAuthenticator
// dispatch. Uses fakes for HttpClient and the VariableResolver from
// the domain layer directly, so no mock SUT process is required.
#include "application/AuthStrategy.h"

#include "domain/VariableResolver.h"
#include "infrastructure/http/HttpClient.h"

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/RunContext.h>

#include <gtest/gtest.h>

#include <expected>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ce = chainapi::engine;

namespace {

/// In-memory fake HTTP client. Replays a queued response per send()
/// call. Records the request URL + body for assertion.
class FakeHttpClient final : public ce::HttpClient {
public:
    struct Recorded {
        std::string url;
        std::string body;
    };

    void enqueue(int status, std::string body,
                 std::vector<std::pair<std::string, std::string>> headers = {}) {
        ce::HttpResponse resp;
        resp.status = status;
        resp.body = std::move(body);
        resp.headers = std::move(headers);
        responses_.push_back(std::move(resp));
    }

    std::expected<ce::HttpResponse, ce::ChainApiError>
    send(const ce::HttpRequest& req) override {
        recorded_.push_back({req.url, req.body.value_or("")});
        if (responses_.empty()) {
            return std::unexpected(ce::ChainApiError{
                ce::ErrorCode::NetworkTimeout, ce::ErrorClass::Network,
                "fake: queue exhausted"});
        }
        auto resp = std::move(responses_.front());
        responses_.erase(responses_.begin());
        return resp;
    }

    [[nodiscard]] const std::vector<Recorded>& recorded() const noexcept {
        return recorded_;
    }

private:
    std::vector<ce::HttpResponse> responses_;
    std::vector<Recorded> recorded_;
};

ce::Actor makeSimpleActor() {
    ce::Actor actor;
    actor.id = ce::ActorId{"user"};
    actor.strategy = ce::AuthStrategy::Simple;

    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/api/v1/auth/login";
    step.bodyTemplate = R"({email:"u@x.test"})";
    step.expectStatus = 200;
    step.extractions.push_back(
        {"token", "$.data.accessToken", ce::Extraction::Source::JsonPath});
    actor.authSteps.push_back(std::move(step));
    return actor;
}

ce::Actor makeChainActor() {
    ce::Actor actor;
    actor.id = ce::ActorId{"customer"};
    actor.strategy = ce::AuthStrategy::Chain;

    ce::AuthStep send;
    send.id = "send_otp";
    send.method = ce::HttpMethod::Post;
    send.pathTemplate = "/api/v1/auth/send-otp";
    send.expectStatus = 200;

    ce::AuthStep verify;
    verify.id = "verify_otp";
    verify.method = ce::HttpMethod::Post;
    verify.pathTemplate = "/api/v1/auth/verify-otp";
    verify.expectStatus = 200;
    verify.extractions.push_back(
        {"token", "$.data.accessToken", ce::Extraction::Source::JsonPath});

    actor.authSteps.push_back(std::move(send));
    actor.authSteps.push_back(std::move(verify));
    return actor;
}

ce::ResolveContext makeRctx() {
    ce::ResolveContext rctx;
    rctx.envVars = {{"baseUrl", "http://t.test"}};
    return rctx;
}

}  // namespace

TEST(AuthStrategy, simple_strategy_authenticates_via_single_step) {
    FakeHttpClient http;
    http.enqueue(200, R"({"data":{"accessToken":"tok-abc"}})");

    ce::VariableResolver resolver;
    auto auther = ce::selectAuthenticator(
        makeSimpleActor(),
        ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(makeSimpleActor(), ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("token"), "tok-abc");
    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].url, "http://t.test/api/v1/auth/login");
}

TEST(AuthStrategy, chain_strategy_runs_steps_in_order) {
    FakeHttpClient http;
    http.enqueue(200, R"({"ok":true})");                                 // send_otp
    http.enqueue(200, R"({"data":{"accessToken":"tok-xyz"}})");          // verify_otp

    ce::VariableResolver resolver;
    auto auther = ce::selectAuthenticator(
        makeChainActor(),
        ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(makeChainActor(), ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("token"), "tok-xyz");
    ASSERT_EQ(http.recorded().size(), 2u);
    EXPECT_EQ(http.recorded()[0].url, "http://t.test/api/v1/auth/send-otp");
    EXPECT_EQ(http.recorded()[1].url, "http://t.test/api/v1/auth/verify-otp");
}

TEST(AuthStrategy, status_mismatch_surfaces_session_refresh_failed) {
    // Locks in the contract that integration tests rely on: any auth
    // failure surfaces as SessionRefreshFailed regardless of HTTP code.
    FakeHttpClient http;
    http.enqueue(500, R"({"err":"oops"})");

    ce::VariableResolver resolver;
    auto auther = ce::selectAuthenticator(
        makeSimpleActor(),
        ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(makeSimpleActor(), ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_EQ(result.error().cls,  ce::ErrorClass::Auth);
    EXPECT_NE(result.error().detail.find("HTTP 500"), std::string::npos);
}

TEST(AuthStrategy, network_failure_surfaces_session_refresh_failed) {
    FakeHttpClient http;  // empty queue — every send() returns NetworkTimeout

    ce::VariableResolver resolver;
    auto auther = ce::selectAuthenticator(
        makeSimpleActor(),
        ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(makeSimpleActor(), ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
}

TEST(AuthStrategy, extraction_miss_surfaces_session_refresh_failed) {
    // Server returned 200 but the payload doesn't have the field the
    // extractor expects. Auth must fail (we have no token to work with).
    FakeHttpClient http;
    http.enqueue(200, R"({"data":{}})");

    ce::VariableResolver resolver;
    auto auther = ce::selectAuthenticator(
        makeSimpleActor(),
        ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(makeSimpleActor(), ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("extraction failed"),
              std::string::npos);
}

TEST(AuthStrategy, simple_and_chain_route_to_the_same_authenticator) {
    // Both Simple and Chain map to the same concrete authenticator
    // (the only difference is authSteps.size()). Assert both behave
    // identically against the same input rather than using typeid
    // (which triggers -Wpotentially-evaluated-expression on polymorphic refs).
    FakeHttpClient h1;
    FakeHttpClient h2;
    h1.enqueue(200, R"({"data":{"accessToken":"t1"}})");
    h2.enqueue(200, R"({"data":{"accessToken":"t2"}})");

    ce::VariableResolver resolver;
    auto a1 = ce::selectAuthenticator(makeSimpleActor(),
                                      ce::AuthDependencies{&h1, &resolver});
    auto a2 = ce::selectAuthenticator(makeChainActor(),
                                      ce::AuthDependencies{&h2, &resolver});
    ASSERT_NE(a1, nullptr);
    ASSERT_NE(a2, nullptr);

    // Even though the chain actor has 2 steps and only 1 response is
    // queued, that's fine for this test because we only care that both
    // returned authenticators implement the same interface and produce
    // a session shape consistent with the queued response.
    h2.enqueue(200, R"({"data":{"accessToken":"t2"}})");

    ce::RunContext c1;
    ce::RunContext c2;
    auto r1 = a1->authenticate(makeSimpleActor(), c1, makeRctx());
    auto r2 = a2->authenticate(makeChainActor(),  c2, makeRctx());

    ASSERT_TRUE(r1.has_value()) << r1.error().detail;
    ASSERT_TRUE(r2.has_value()) << r2.error().detail;
    EXPECT_EQ(r1->variables.at("token"), "t1");
    EXPECT_EQ(r2->variables.at("token"), "t2");
}


// ───  BasicAuthenticator ──────────────────────────────────────────

namespace {

ce::Actor makeBasicActor(std::string username, std::string password) {
    ce::Actor actor;
    actor.id = ce::ActorId{"client"};
    actor.strategy = ce::AuthStrategy::Basic;
    actor.authConfig = {{"username", std::move(username)},
                        {"password", std::move(password)}};
    actor.inject.headers["Authorization"] = "Basic {{client.credential}}";
    return actor;
}

}  // namespace

TEST(AuthStrategy, basic_emits_rfc7617_canonical_credential) {
    // RFC 7617 §2 example: "Aladdin" / "open sesame" → "QWxhZGRpbjpvcGVuIHNlc2FtZQ==".
    FakeHttpClient http;  // intentionally empty — Basic makes no HTTP call
    ce::VariableResolver resolver;

    auto actor = makeBasicActor("Aladdin", "open sesame");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("credential"),
              "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    // No HTTP call happened.
    EXPECT_TRUE(http.recorded().empty());
}

TEST(AuthStrategy, basic_resolves_secret_references_in_credentials) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeBasicActor("{{secret.API_USER}}", "{{secret.API_PASS}}");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto rctx = makeRctx();
    rctx.secrets["API_USER"] = "alice";
    rctx.secrets["API_PASS"] = "wonderland";

    auto result = auther->authenticate(actor, ctx, rctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    // alice:wonderland → YWxpY2U6d29uZGVybGFuZA==
    EXPECT_EQ(result->variables.at("credential"),
              "YWxpY2U6d29uZGVybGFuZA==");
}

TEST(AuthStrategy, basic_missing_credentials_surface_session_refresh_failed) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    ce::Actor actor;
    actor.id = ce::ActorId{"client"};
    actor.strategy = ce::AuthStrategy::Basic;
    actor.authConfig = {{"username", "alice"}};  // password missing

    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("password"), std::string::npos);
}

TEST(AuthStrategy, basic_unresolved_variable_surfaces_session_refresh_failed) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeBasicActor("{{secret.MISSING}}", "x");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("unresolved"), std::string::npos);
}


// ─── ApiKeyAuthenticator ─────────────────────────────────────────────────────
namespace {

ce::Actor makeApiKeyActor(std::string key,
                          std::optional<std::string> location = std::nullopt,
                          std::optional<std::string> name = std::nullopt) {
    ce::Actor actor;
    actor.id = ce::ActorId{"service"};
    actor.strategy = ce::AuthStrategy::ApiKey;
    actor.authConfig["key"] = std::move(key);
    if (location) actor.authConfig["location"] = std::move(*location);
    if (name)     actor.authConfig["name"]     = std::move(*name);
    return actor;
}

}  // namespace

TEST(AuthStrategy, api_key_stores_resolved_value_as_session_variable) {
    // Manual-inject path: no `location`/`name` → strategy only exposes
    // the variable. The user wires it themselves via `inject:`.
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("{{secret.SERVICE_API_KEY}}");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto rctx = makeRctx();
    rctx.secrets["SERVICE_API_KEY"] = "sk_live_abc";

    auto result = auther->authenticate(actor, ctx, rctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("key"), "sk_live_abc");
    EXPECT_TRUE(result->injectHeaders.empty());
    EXPECT_TRUE(result->injectQueryParams.empty());
    EXPECT_TRUE(http.recorded().empty());  // no HTTP call
}

TEST(AuthStrategy, api_key_header_location_auto_injects_into_session) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("sk_live_abc", "header", "X-API-Key");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("key"), "sk_live_abc");
    // Auto-inject populated.
    ASSERT_EQ(result->injectHeaders.size(), 1u);
    EXPECT_EQ(result->injectHeaders.at("X-API-Key"), "sk_live_abc");
    EXPECT_TRUE(result->injectQueryParams.empty());
}

TEST(AuthStrategy, api_key_query_location_auto_injects_query_param) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("sk_live_abc", "query", "api_key");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->injectQueryParams.at("api_key"), "sk_live_abc");
    EXPECT_TRUE(result->injectHeaders.empty());
}

TEST(AuthStrategy, api_key_cookie_location_is_explicitly_unsupported) {
    // Cookie jars are post-MVP. The strategy must reject this rather
    // than silently producing nothing; a clean error message points
    // the user at the manual `inject:` workaround.
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("sk_live_abc", "cookie", "session");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("cookie"), std::string::npos);
}

TEST(AuthStrategy, api_key_unknown_location_is_a_clear_error) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("sk_live_abc", "body", "X-Key");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("location"), std::string::npos);
}

TEST(AuthStrategy, api_key_missing_key_surfaces_session_refresh_failed) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    ce::Actor actor;
    actor.id = ce::ActorId{"service"};
    actor.strategy = ce::AuthStrategy::ApiKey;
    // No `key` configured.

    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("key"), std::string::npos);
}

TEST(AuthStrategy, api_key_location_without_name_does_not_auto_inject) {
    // Hybrid contract: BOTH `location` and `name` must be set for
    // auto-inject to fire. With only one, the strategy still exposes
    // the variable so the user can wire `inject:` manually — no
    // surprise auto-inject under a default name.
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeApiKeyActor("sk_live_abc", "header" /* name omitted */);
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("key"), "sk_live_abc");
    EXPECT_TRUE(result->injectHeaders.empty())
        << "auto-inject should require both location AND name";
}


// ─── OAuth2ClientCredentialsAuthenticator ────────────────────────────────────

namespace {

ce::Actor makeOAuth2ClientCredsActor(
    std::string tokenUrl,
    std::string clientId,
    std::string clientSecret,
    std::optional<std::string> scope = std::nullopt) {
    ce::Actor actor;
    actor.id = ce::ActorId{"service"};
    actor.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    actor.authConfig["token_url"]     = std::move(tokenUrl);
    actor.authConfig["client_id"]     = std::move(clientId);
    actor.authConfig["client_secret"] = std::move(clientSecret);
    if (scope) actor.authConfig["scope"] = std::move(*scope);
    return actor;
}

}  // namespace

TEST(AuthStrategy, oauth2_client_credentials_extracts_bearer_and_auto_injects) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"tok-XYZ","token_type":"Bearer",
                          "expires_in":3600,"scope":"read write"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "https://idp.test/oauth/token", "client-1", "secret-1",
        "read write");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("access_token"), "tok-XYZ");
    EXPECT_EQ(result->variables.at("token_type"),  "Bearer");
    EXPECT_EQ(result->variables.at("expires_in"),  "3600");
    EXPECT_EQ(result->variables.at("scope"),       "read write");
    // Auto-inject populated.
    EXPECT_EQ(result->injectHeaders.at("Authorization"),
              "Bearer tok-XYZ");

    // The strategy made exactly one POST to the token endpoint with
    // the correct form body.
    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].url, "https://idp.test/oauth/token");
    const auto& body = http.recorded()[0].body;
    EXPECT_NE(body.find("grant_type=client_credentials"), std::string::npos);
    EXPECT_NE(body.find("client_id=client-1"), std::string::npos);
    EXPECT_NE(body.find("client_secret=secret-1"), std::string::npos);
    EXPECT_NE(body.find("scope=read%20write"), std::string::npos);
}

TEST(AuthStrategy, oauth2_client_credentials_resolves_secret_references) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"tok-Z"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "{{env.idp_url}}",
        "{{secret.OAUTH_CLIENT_ID}}",
        "{{secret.OAUTH_CLIENT_SECRET}}");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto rctx = makeRctx();
    rctx.envVars["idp_url"] = "https://idp.test/oauth/token";
    rctx.secrets["OAUTH_CLIENT_ID"]     = "id-from-keychain";
    rctx.secrets["OAUTH_CLIENT_SECRET"] = "secret-from-keychain";

    auto result = auther->authenticate(actor, ctx, rctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("access_token"), "tok-Z");

    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].url, "https://idp.test/oauth/token");
    const auto& body = http.recorded()[0].body;
    EXPECT_NE(body.find("client_id=id-from-keychain"),     std::string::npos);
    EXPECT_NE(body.find("client_secret=secret-from-keychain"), std::string::npos);
}

TEST(AuthStrategy, oauth2_client_credentials_omits_scope_when_not_configured) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"tok-A"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "https://idp.test/token", "id", "sec");  // no scope
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].body.find("scope="), std::string::npos);
}

TEST(AuthStrategy, oauth2_client_credentials_rejects_missing_client_id) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    ce::Actor actor;
    actor.id = ce::ActorId{"service"};
    actor.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    actor.authConfig["token_url"]     = "https://idp.test/token";
    actor.authConfig["client_secret"] = "sec";
    // client_id missing entirely

    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("client_id"), std::string::npos);
    EXPECT_TRUE(http.recorded().empty())
        << "no HTTP call must happen when required field is missing";
}

TEST(AuthStrategy, oauth2_client_credentials_surfaces_token_endpoint_error) {
    // RFC 6749 §5.2 token-endpoint error responses use 400 with an
    // `error` field in the body. The strategy must surface enough of
    // that body for the user to debug.
    FakeHttpClient http;
    http.enqueue(400,
        R"({"error":"invalid_client","error_description":"Bad credentials"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "https://idp.test/token", "client-x", "wrong-secret");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("HTTP 400"), std::string::npos);
    EXPECT_NE(result.error().detail.find("invalid_client"),
              std::string::npos);
}

TEST(AuthStrategy, oauth2_client_credentials_rejects_response_without_access_token) {
    FakeHttpClient http;
    http.enqueue(200, R"({"token_type":"Bearer"})");  // no access_token

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "https://idp.test/token", "id", "sec");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("access_token"),
              std::string::npos);
}

TEST(AuthStrategy, oauth2_client_credentials_network_error_surfaces_cleanly) {
    FakeHttpClient http;  // empty queue → NetworkTimeout

    ce::VariableResolver resolver;
    auto actor = makeOAuth2ClientCredsActor(
        "https://idp.test/token", "id", "sec");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("network"), std::string::npos);
}


// ─── OAuth2PasswordAuthenticator (RFC 6749 §4.3) ─────────────────────────────

namespace {

ce::Actor makeOAuth2PasswordActor(
    std::string tokenUrl,
    std::string clientId,
    std::string clientSecret,
    std::string username,
    std::string password,
    std::optional<std::string> scope = std::nullopt) {
    ce::Actor actor;
    actor.id = ce::ActorId{"user"};
    actor.strategy = ce::AuthStrategy::OAuth2Password;
    actor.authConfig["token_url"]     = std::move(tokenUrl);
    actor.authConfig["client_id"]     = std::move(clientId);
    actor.authConfig["client_secret"] = std::move(clientSecret);
    actor.authConfig["username"]      = std::move(username);
    actor.authConfig["password"]      = std::move(password);
    if (scope) actor.authConfig["scope"] = std::move(*scope);
    return actor;
}

}  // namespace

TEST(AuthStrategy, oauth2_password_extracts_bearer_and_auto_injects) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"pwd-tok","token_type":"Bearer",
                          "expires_in":7200,"refresh_token":"rt-1"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2PasswordActor(
        "https://idp.test/oauth/token",
        "client-id", "client-secret",
        "alice", "wonderland",
        "read:notes");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("access_token"),  "pwd-tok");
    EXPECT_EQ(result->variables.at("token_type"),   "Bearer");
    EXPECT_EQ(result->variables.at("expires_in"),   "7200");
    EXPECT_EQ(result->variables.at("refresh_token"),"rt-1");
    EXPECT_EQ(result->injectHeaders.at("Authorization"),
              "Bearer pwd-tok");

    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].url, "https://idp.test/oauth/token");
    const auto& body = http.recorded()[0].body;
    EXPECT_NE(body.find("grant_type=password"),       std::string::npos);
    EXPECT_NE(body.find("username=alice"),            std::string::npos);
    EXPECT_NE(body.find("password=wonderland"),       std::string::npos);
    EXPECT_NE(body.find("client_id=client-id"),       std::string::npos);
    EXPECT_NE(body.find("client_secret=client-secret"), std::string::npos);
    EXPECT_NE(body.find("scope=read%3Anotes"),        std::string::npos);
}

TEST(AuthStrategy, oauth2_password_resolves_secret_username_and_password) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"pwd-tok"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2PasswordActor(
        "https://idp.test/oauth/token",
        "id", "sec",
        "{{secret.RO_USER}}",
        "{{secret.RO_PASS}}");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto rctx = makeRctx();
    rctx.secrets["RO_USER"] = "real-alice";
    rctx.secrets["RO_PASS"] = "real-secret";

    auto result = auther->authenticate(actor, ctx, rctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(http.recorded().size(), 1u);
    const auto& body = http.recorded()[0].body;
    EXPECT_NE(body.find("username=real-alice"),  std::string::npos);
    EXPECT_NE(body.find("password=real-secret"), std::string::npos);
    EXPECT_EQ(body.find("{{secret"), std::string::npos)
        << "secret references must not survive into the wire";
}

TEST(AuthStrategy, oauth2_password_url_encodes_special_chars_in_credentials) {
    // Defensive: passwords containing reserved chars must round-trip
    // through url-encoding, otherwise the token endpoint would parse
    // them wrong (e.g. `&` ending the field early).
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"x"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2PasswordActor(
        "https://idp.test/token", "id", "sec",
        "user@example.com",   // `@` requires %40
        "p&w=hard!");          // `&` `=` `!`
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(http.recorded().size(), 1u);
    const auto& body = http.recorded()[0].body;
    EXPECT_NE(body.find("username=user%40example.com"), std::string::npos);
    EXPECT_NE(body.find("password=p%26w%3Dhard%21"),    std::string::npos);
}

TEST(AuthStrategy, oauth2_password_rejects_missing_username) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    ce::Actor actor;
    actor.id = ce::ActorId{"user"};
    actor.strategy = ce::AuthStrategy::OAuth2Password;
    actor.authConfig = {
        {"token_url",     "https://idp.test/token"},
        {"client_id",     "id"},
        {"client_secret", "sec"},
        // username deliberately absent
        {"password",      "x"},
    };

    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("username"), std::string::npos);
    EXPECT_TRUE(http.recorded().empty());
}

TEST(AuthStrategy, oauth2_password_surfaces_token_endpoint_error) {
    FakeHttpClient http;
    http.enqueue(401,
        R"({"error":"invalid_grant","error_description":"Bad password"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2PasswordActor(
        "https://idp.test/token", "id", "sec", "alice", "wrong");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("HTTP 401"), std::string::npos);
    EXPECT_NE(result.error().detail.find("invalid_grant"),
              std::string::npos);
}

TEST(AuthStrategy, oauth2_password_omits_scope_when_not_configured) {
    FakeHttpClient http;
    http.enqueue(200, R"({"access_token":"x"})");

    ce::VariableResolver resolver;
    auto actor = makeOAuth2PasswordActor(
        "https://idp.test/token", "id", "sec", "alice", "pw");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());
    ASSERT_TRUE(result.has_value());

    ASSERT_EQ(http.recorded().size(), 1u);
    EXPECT_EQ(http.recorded()[0].body.find("scope="), std::string::npos);
}


// ─── Slice 4f — OAuth1 (RFC 5849 HMAC-SHA1) ─────────────────────────────────

#include "application/RequestSigners.h"

namespace {

ce::Actor makeOAuth1Actor(std::string consumerKey,
                          std::string consumerSecret,
                          std::optional<std::string> token = std::nullopt,
                          std::optional<std::string> tokenSecret = std::nullopt,
                          std::optional<std::string> realm = std::nullopt) {
    ce::Actor actor;
    actor.id = ce::ActorId{"twitter"};
    actor.strategy = ce::AuthStrategy::OAuth1;
    actor.authConfig["consumer_key"]    = std::move(consumerKey);
    actor.authConfig["consumer_secret"] = std::move(consumerSecret);
    if (token)       actor.authConfig["token"]        = std::move(*token);
    if (tokenSecret) actor.authConfig["token_secret"] = std::move(*tokenSecret);
    if (realm)       actor.authConfig["realm"]        = std::move(*realm);
    return actor;
}

}  // namespace

TEST(AuthStrategy, oauth1_authenticator_populates_session_and_marks_signing) {
    FakeHttpClient http;  // intentionally empty — OAuth1 makes no auth call
    ce::VariableResolver resolver;

    auto actor = makeOAuth1Actor(
        "{{secret.OAUTH_KEY}}",
        "{{secret.OAUTH_SECRET}}",
        "{{secret.OAUTH_TOKEN}}",
        "{{secret.OAUTH_TOKEN_SECRET}}",
        "Realm");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});
    ASSERT_NE(auther, nullptr);

    ce::RunContext ctx;
    auto rctx = makeRctx();
    rctx.secrets["OAUTH_KEY"]          = "9djdj82h48djs9d2";
    rctx.secrets["OAUTH_SECRET"]       = "j49sk3j29djd";
    rctx.secrets["OAUTH_TOKEN"]        = "kkk9d7dh3k39sjv7";
    rctx.secrets["OAUTH_TOKEN_SECRET"] = "dh893hdasih9";

    auto result = auther->authenticate(actor, ctx, rctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_EQ(result->variables.at("consumer_key"),    "9djdj82h48djs9d2");
    EXPECT_EQ(result->variables.at("consumer_secret"), "j49sk3j29djd");
    EXPECT_EQ(result->variables.at("token"),           "kkk9d7dh3k39sjv7");
    EXPECT_EQ(result->variables.at("token_secret"),    "dh893hdasih9");
    EXPECT_EQ(result->variables.at("realm"),           "Realm");

    // OAuth1 signs per-request, not at auth time. The session must
    // carry the signing-scheme flag so the executor calls the signer.
    EXPECT_EQ(result->signingScheme,
              ce::ActorSession::SigningScheme::OAuth1HmacSha1);
    EXPECT_TRUE(http.recorded().empty()) << "OAuth1 must make no HTTP call";
}

TEST(AuthStrategy, oauth1_authenticator_supports_two_legged_signing) {
    // Two-legged: just consumer credentials, no token. RFC 5849 §3.1.
    FakeHttpClient http;
    ce::VariableResolver resolver;

    auto actor = makeOAuth1Actor("ck", "cs");
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->variables.at("consumer_key"),    "ck");
    EXPECT_FALSE(result->variables.contains("token"));
    EXPECT_FALSE(result->variables.contains("token_secret"));
    EXPECT_EQ(result->signingScheme,
              ce::ActorSession::SigningScheme::OAuth1HmacSha1);
}

TEST(AuthStrategy, oauth1_authenticator_rejects_token_without_secret) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    // Token set, token_secret missing — must fail cleanly rather than
    // silently producing bad signatures.
    auto actor = makeOAuth1Actor("ck", "cs", "tok"  /* tokenSecret omitted */);
    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("token_secret"), std::string::npos);
}

TEST(AuthStrategy, oauth1_authenticator_rejects_missing_consumer_key) {
    FakeHttpClient http;
    ce::VariableResolver resolver;

    ce::Actor actor;
    actor.id = ce::ActorId{"twitter"};
    actor.strategy = ce::AuthStrategy::OAuth1;
    actor.authConfig["consumer_secret"] = "cs";
    // consumer_key absent

    auto auther = ce::selectAuthenticator(
        actor, ce::AuthDependencies{&http, &resolver});

    ce::RunContext ctx;
    auto result = auther->authenticate(actor, ctx, makeRctx());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SessionRefreshFailed);
    EXPECT_NE(result.error().detail.find("consumer_key"), std::string::npos);
}

// ─── signOAuth1Request — RFC 5849 §3.4.3 reference vector ────────────────────

TEST(OAuth1Signer, matches_rfc5849_section_3_4_3_reference_vector) {
    // Worked example from RFC 5849 Appendix-equivalent section 3.4.3:
    //   consumer_key    = 9djdj82h48djs9d2
    //   consumer_secret = j49sk3j29djd
    //   token           = kkk9d7dh3k39sjv7
    //   token_secret    = dh893hdasih9
    //   method = POST
    //   url    = http://example.com/request?b5=%3D%253D&a3=a&c%40=&a2=r%20b
    //   body   = c2&a3=2+q   (application/x-www-form-urlencoded)
    //   nonce  = 7d8f3e4a    (forced via test override)
    //   ts     = 137131201   (forced via test override)
    //
    // Expected signature (Base64-encoded HMAC-SHA1):
    //   r6/TJjbCOr97/+UU0NsvSne7s5g=
    //
    // Source: RFC 5849 §3.4.3 Appendix worked example (the canonical
    // one most OAuth1 libraries pin to).
    ce::ActorSession session;
    session.variables["consumer_key"]    = "9djdj82h48djs9d2";
    session.variables["consumer_secret"] = "j49sk3j29djd";
    session.variables["token"]           = "kkk9d7dh3k39sjv7";
    session.variables["token_secret"]    = "dh893hdasih9";
    session.signingScheme =
        ce::ActorSession::SigningScheme::OAuth1HmacSha1;

    ce::HttpRequest req;
    req.method = ce::HttpMethod::Post;
    req.url = "http://example.com/request"
              "?b5=%3D%253D&a3=a&c%40=&a2=r%20b";
    req.body = "c2&a3=2+q";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";

    ce::OAuth1TestOverrides overrides;
    overrides.nonce = "7d8f3e4a";
    overrides.timestampSeconds = "137131201";

    const bool ok = ce::signOAuth1Request(req, session, overrides);
    ASSERT_TRUE(ok);

    const auto& auth = req.headers.at("Authorization");
    ASSERT_TRUE(auth.starts_with("OAuth ")) << "got: " << auth;

    // Pin the exact signature from the RFC. URL-encoding of '=' is
    // %3D and of '+' is %2B; the encoded form in the header is what
    // we assert on.
    EXPECT_NE(auth.find(R"(oauth_signature="r6%2FTJjbCOr97%2F%2BUU0NsvSne7s5g%3D")"),
              std::string::npos)
        << "auth header: " << auth;

    // Sanity: nonce + timestamp are echoed verbatim, signature method
    // is HMAC-SHA1. Per RFC 5849 §3.4.3 worked example, oauth_version
    // is intentionally omitted; matching it interops with the
    // canonical signature.
    EXPECT_NE(auth.find(R"(oauth_nonce="7d8f3e4a")"), std::string::npos);
    EXPECT_NE(auth.find(R"(oauth_timestamp="137131201")"), std::string::npos);
    EXPECT_NE(auth.find(R"(oauth_signature_method="HMAC-SHA1")"),
              std::string::npos);
    EXPECT_EQ(auth.find("oauth_version"), std::string::npos)
        << "RFC 5849 §3.4.3 worked example omits oauth_version";
    EXPECT_NE(auth.find(R"(oauth_consumer_key="9djdj82h48djs9d2")"),
              std::string::npos);
    EXPECT_NE(auth.find(R"(oauth_token="kkk9d7dh3k39sjv7")"), std::string::npos);
}

TEST(OAuth1Signer, two_legged_signs_with_empty_token_secret) {
    // Two-legged signing key per RFC 5849 §3.4.2 is
    // `encode(consumer_secret) & encode("")` — the trailing & is
    // mandatory. Pinning the exact signature here would require
    // pre-computing it externally; the lighter contract this test
    // captures is "signing succeeds, header is well-formed, no
    // oauth_token field is emitted".
    ce::ActorSession session;
    session.variables["consumer_key"]    = "ck";
    session.variables["consumer_secret"] = "cs";

    ce::HttpRequest req;
    req.method = ce::HttpMethod::Get;
    req.url = "https://api.example.test/v1/things";

    ce::OAuth1TestOverrides overrides;
    overrides.nonce = "fixed-nonce";
    overrides.timestampSeconds = "1700000000";

    const bool ok = ce::signOAuth1Request(req, session, overrides);
    ASSERT_TRUE(ok);

    const auto& auth = req.headers.at("Authorization");
    EXPECT_TRUE(auth.starts_with("OAuth "));
    EXPECT_EQ(auth.find("oauth_token="), std::string::npos)
        << "two-legged signing must omit oauth_token";
    EXPECT_NE(auth.find(R"(oauth_consumer_key="ck")"), std::string::npos);
    EXPECT_NE(auth.find("oauth_signature="), std::string::npos);
}

TEST(OAuth1Signer, regenerates_distinct_signatures_across_calls) {
    // Per-attempt nonce regeneration: two consecutive sign calls on
    // the same request must produce different Authorization headers
    // (different nonce → different signature).
    ce::ActorSession session;
    session.variables["consumer_key"]    = "ck";
    session.variables["consumer_secret"] = "cs";

    ce::HttpRequest reqA;
    reqA.method = ce::HttpMethod::Get;
    reqA.url = "https://api.example.test/v1/things";

    ce::HttpRequest reqB = reqA;

    ASSERT_TRUE(ce::signOAuth1Request(reqA, session));
    ASSERT_TRUE(ce::signOAuth1Request(reqB, session));

    EXPECT_NE(reqA.headers.at("Authorization"),
              reqB.headers.at("Authorization"));
}

TEST(OAuth1Signer, refuses_to_sign_when_consumer_credentials_missing) {
    ce::ActorSession session;
    // No consumer_key / consumer_secret in variables.

    ce::HttpRequest req;
    req.method = ce::HttpMethod::Get;
    req.url = "https://api.example.test/v1/things";

    EXPECT_FALSE(ce::signOAuth1Request(req, session));
    EXPECT_EQ(req.headers.find("Authorization"), req.headers.end());
}

TEST(OAuth1Signer, includes_realm_in_header_when_present) {
    ce::ActorSession session;
    session.variables["consumer_key"]    = "ck";
    session.variables["consumer_secret"] = "cs";
    session.variables["realm"]           = "Photos";

    ce::HttpRequest req;
    req.method = ce::HttpMethod::Get;
    req.url = "https://photos.example.com/v1/list";

    ASSERT_TRUE(ce::signOAuth1Request(req, session));
    const auto& auth = req.headers.at("Authorization");
    EXPECT_NE(auth.find(R"(realm="Photos")"), std::string::npos);
}
