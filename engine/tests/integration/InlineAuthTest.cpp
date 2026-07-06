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
