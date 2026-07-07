// InlineAuthTest — end-to-end proof that per-operation inline (actor-less)
// auth reaches the wire. Runs ops carrying Bearer / Basic / API-key inline
// auth against the mock SUT and asserts the server received the expected
// Authorization / API-key header. Fails on the parent commit: Operation had
// no inlineAuth field and the executor injected nothing.
#include "MockSutHarness.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

/// Read /__mock/last-request?path=<p> via a tiny curl helper (same pattern as
/// MultipartTest — avoids piercing the engine's private HttpClient).
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

[[nodiscard]] ce::Operation makeOp(const std::string& name,
                                   const std::string& path,
                                   ce::InlineAuth auth) {
    ce::Operation op;
    op.id = ce::OperationId{"echo." + name};
    op.resource = ce::ResourceId{"echo"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = path;
    op.expectStatus = 200;
    op.inlineAuth = std::move(auth);
    return op;
}

[[nodiscard]] ce::Project makeProject(const std::string& baseUrl) {
    ce::Project p;
    p.name = "inline-auth";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", baseUrl}};

    ce::Resource echo;
    echo.id = ce::ResourceId{"echo"};
    echo.operations["bearer"] =
        makeOp("bearer",
               "/bearer",
               ce::InlineAuth{.type = ce::InlineAuthType::Bearer, .token = "tok-abc"});
    echo.operations["basic"] =
        makeOp("basic",
               "/basic",
               ce::InlineAuth{
                   .type = ce::InlineAuthType::Basic, .username = "admin", .password = "secret"});
    echo.operations["apikey"] = makeOp("apikey",
                                       "/apikey",
                                       ce::InlineAuth{.type = ce::InlineAuthType::ApiKey,
                                                      .apiKeyName = "X-API-Key",
                                                      .apiKeyValue = "key-xyz",
                                                      .apiKeyInQuery = false});
    echo.operations["aws"] = makeOp("aws",
                                    "/aws",
                                    ce::InlineAuth{.type = ce::InlineAuthType::AwsSigV4,
                                                   .awsAccessKey = "AKIDEXAMPLE",
                                                   .awsSecretKey = "wJalrXUtnFEMI/K7MDENG",
                                                   .awsRegion = "us-east-1",
                                                   .awsService = "service"});
    echo.operations["oauth"] = makeOp("oauth",
                                      "/oauth",
                                      ce::InlineAuth{.type = ce::InlineAuthType::OAuth1,
                                                     .oauthConsumerKey = "ck",
                                                     .oauthConsumerSecret = "cs",
                                                     .oauthToken = "tk",
                                                     .oauthTokenSecret = "ts"});
    echo.operations["jwt"] = makeOp("jwt",
                                    "/jwt",
                                    ce::InlineAuth{.type = ce::InlineAuthType::Jwt,
                                                   .jwtAlgorithm = "HS256",
                                                   .jwtSecret = "topsecret",
                                                   .jwtPayload = R"({"sub":"abc"})"});
    p.resources[ce::ResourceId{"echo"}] = std::move(echo);
    return p;
}

}  // namespace

class InlineAuthFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "inline-auth-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(InlineAuthFixture, bearer_token_reaches_the_wire) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.bearer"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/bearer");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["headers"]["authorization"].get<std::string>(), "Bearer tok-abc");
}

TEST_F(InlineAuthFixture, basic_auth_is_base64_encoded) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.basic"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/basic");
    ASSERT_TRUE(cap["found"].get<bool>());
    // base64("admin:secret") == "YWRtaW46c2VjcmV0"
    EXPECT_EQ(cap["headers"]["authorization"].get<std::string>(), "Basic YWRtaW46c2VjcmV0");
}

TEST_F(InlineAuthFixture, api_key_header_reaches_the_wire) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.apikey"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/apikey");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["headers"]["x-api-key"].get<std::string>(), "key-xyz");
}

TEST_F(InlineAuthFixture, aws_sigv4_signs_the_request) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.aws"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    // Exact-signature correctness is covered by RequestSigners unit tests;
    // here we prove the inline path invoked the signer end-to-end.
    auto cap = fetchLastRequest(harness_->baseUrl(), "/aws");
    ASSERT_TRUE(cap["found"].get<bool>());
    const auto auth = cap["headers"]["authorization"].get<std::string>();
    EXPECT_TRUE(auth.starts_with("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/")) << auth;
    EXPECT_TRUE(cap["headers"].contains("x-amz-date"));
}

TEST_F(InlineAuthFixture, oauth1_signs_the_request) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.oauth"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/oauth");
    ASSERT_TRUE(cap["found"].get<bool>());
    const auto auth = cap["headers"]["authorization"].get<std::string>();
    EXPECT_TRUE(auth.starts_with("OAuth ")) << auth;
    EXPECT_NE(auth.find("oauth_consumer_key=\"ck\""), std::string::npos) << auth;
    EXPECT_NE(auth.find("oauth_signature="), std::string::npos) << auth;
}

TEST_F(InlineAuthFixture, jwt_bearer_injects_a_signed_token) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.jwt"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/jwt");
    ASSERT_TRUE(cap["found"].get<bool>());
    const auto auth = cap["headers"]["authorization"].get<std::string>();
    // base64url({"alg":"HS256","typ":"JWT"}) == "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    EXPECT_TRUE(auth.starts_with("Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.")) << auth;
}

TEST_F(InlineAuthFixture, oauth2_fetches_a_token_and_injects_bearer) {
    auto project = makeProject(harness_->baseUrl());
    // Point the op at the mock token endpoint.
    ce::Resource& echo = project.resources.at(ce::ResourceId{"echo"});
    echo.operations["oauth2"] =
        makeOp("oauth2",
               "/oauth2",
               ce::InlineAuth{.type = ce::InlineAuthType::OAuth2,
                              .oauth2TokenUrl = harness_->baseUrl() + "/token",
                              .oauth2ClientId = "cid",
                              .oauth2ClientSecret = "csecret"});

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"echo.oauth2"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/oauth2");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["headers"]["authorization"].get<std::string>(), "Bearer oauth2-tok");
}

TEST_F(InlineAuthFixture, inherit_uses_the_project_default_auth) {
    auto project = makeProject(harness_->baseUrl());
    project.defaultAuth =
        ce::InlineAuth{.type = ce::InlineAuthType::Bearer, .token = "inherited-tok"};
    ce::Resource& echo = project.resources.at(ce::ResourceId{"echo"});
    echo.operations["inherit"] =
        makeOp("inherit", "/inherit", ce::InlineAuth{.type = ce::InlineAuthType::Inherit});

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"echo.inherit"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/inherit");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["headers"]["authorization"].get<std::string>(), "Bearer inherited-tok");
}

TEST_F(InlineAuthFixture, jwt_unsupported_algorithm_fails_the_step) {
    auto project = makeProject(harness_->baseUrl());
    ce::Resource& echo = project.resources.at(ce::ResourceId{"echo"});
    echo.operations["jwtbad"] = makeOp("jwtbad",
                                       "/jwt",
                                       ce::InlineAuth{.type = ce::InlineAuthType::Jwt,
                                                      .jwtAlgorithm = "RS256",
                                                      .jwtSecret = "s",
                                                      .jwtPayload = R"({"sub":"x"})"});

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"echo.jwtbad"}, ctx);
    ASSERT_TRUE(result.has_value()) << "engine itself returned an error";
    EXPECT_FALSE(result->succeeded());
    bool sawFailure = false;
    for (const auto& step : result->steps) {
        if (step.op.value == "echo.jwtbad" && step.status == ce::StepResult::Status::Failed) {
            EXPECT_NE(step.detail.find("unsupported algorithm"), std::string::npos) << step.detail;
            sawFailure = true;
        }
    }
    EXPECT_TRUE(sawFailure);
}

TEST_F(InlineAuthFixture, oauth2_token_is_cached_across_operations) {
    // Two ops with identical OAuth2 config sharing one RunContext must reuse a
    // single fetched token. The token endpoint returns tok-1 then tok-2; with
    // caching both ops carry tok-1 (only one fetch happened).
    auto project = makeProject(harness_->baseUrl());
    const auto oauth2 = ce::InlineAuth{.type = ce::InlineAuthType::OAuth2,
                                       .oauth2TokenUrl = harness_->baseUrl() + "/token-seq",
                                       .oauth2ClientId = "cid",
                                       .oauth2ClientSecret = "csecret"};
    ce::Resource& echo = project.resources.at(ce::ResourceId{"echo"});
    echo.operations["cachea"] = makeOp("cachea", "/cache-a", oauth2);
    echo.operations["cacheb"] = makeOp("cacheb", "/cache-b", oauth2);

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    ASSERT_TRUE(engine.run(project, ce::OperationId{"echo.cachea"}, ctx));
    ASSERT_TRUE(engine.run(project, ce::OperationId{"echo.cacheb"}, ctx));

    auto capA = fetchLastRequest(harness_->baseUrl(), "/cache-a");
    auto capB = fetchLastRequest(harness_->baseUrl(), "/cache-b");
    ASSERT_TRUE(capA["found"].get<bool>());
    ASSERT_TRUE(capB["found"].get<bool>());
    EXPECT_EQ(capA["headers"]["authorization"].get<std::string>(), "Bearer tok-1");
    EXPECT_EQ(capB["headers"]["authorization"].get<std::string>(), "Bearer tok-1")
        << "second op re-fetched a token instead of reusing the cached one";
}

TEST_F(InlineAuthFixture, oauth2_password_grant_with_basic_client_auth) {
    // Password grant + "Send as Basic Auth header": the token endpoint must
    // receive grant_type=password in the body and the client credentials as a
    // Basic Authorization header (not in the body).
    auto project = makeProject(harness_->baseUrl());
    ce::Resource& echo = project.resources.at(ce::ResourceId{"echo"});
    echo.operations["oauth2pw"] =
        makeOp("oauth2pw",
               "/oauth2",
               ce::InlineAuth{.type = ce::InlineAuthType::OAuth2,
                              .username = "alice",
                              .password = "s3cret",
                              .oauth2GrantType = "password",
                              .oauth2TokenUrl = harness_->baseUrl() + "/token",
                              .oauth2ClientId = "cid",
                              .oauth2ClientSecret = "csecret",
                              .oauth2ClientAuth = "basic"});

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"echo.oauth2pw"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    // The op itself carries the fetched bearer.
    auto op = fetchLastRequest(harness_->baseUrl(), "/oauth2");
    ASSERT_TRUE(op["found"].get<bool>());
    EXPECT_EQ(op["headers"]["authorization"].get<std::string>(), "Bearer oauth2-tok");

    // The token request used Basic client auth + password grant.
    auto tok = fetchLastRequest(harness_->baseUrl(), "/token");
    ASSERT_TRUE(tok["found"].get<bool>());
    // base64("cid:csecret") == "Y2lkOmNzZWNyZXQ="
    EXPECT_EQ(tok["headers"]["authorization"].get<std::string>(), "Basic Y2lkOmNzZWNyZXQ=");
    const auto body = tok["raw_body"].get<std::string>();
    EXPECT_NE(body.find("grant_type=password"), std::string::npos) << body;
    EXPECT_NE(body.find("username=alice"), std::string::npos) << body;
    EXPECT_EQ(body.find("client_secret="), std::string::npos)
        << "client_secret must not be in the body when using Basic client auth: " << body;
}
