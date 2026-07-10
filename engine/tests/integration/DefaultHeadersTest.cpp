// DefaultHeadersTest — proves the auto-generated request headers from
// engine::HttpDefaults (User-Agent, Accept-Encoding, Connection) actually reach
// the wire, and that a caller-supplied header of the same name overrides the
// default. Runs plain GETs against the mock SUT and inspects the captured
// request headers. Fails on the parent commit: CurlHttpClient set no
// User-Agent / Accept-Encoding at all.
#include "MockSutHarness.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HttpDefaults.h>
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
    const auto rc = curl_easy_perform(curl.get());
    EXPECT_EQ(rc, CURLE_OK) << curl_easy_strerror(rc);
    return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
}

[[nodiscard]] ce::Project makeProject(const std::string& baseUrl) {
    ce::Project p;
    p.name = "default-headers";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", baseUrl}};

    ce::Resource echo;
    echo.id = ce::ResourceId{"echo"};

    ce::Operation plain;
    plain.id = ce::OperationId{"echo.plain"};
    plain.resource = echo.id;
    plain.method = ce::HttpMethod::Get;
    plain.pathTemplate = "/plain";
    plain.expectStatus = 200;
    echo.operations["plain"] = std::move(plain);

    // Same request but with a caller-supplied User-Agent — must override ours.
    ce::Operation custom;
    custom.id = ce::OperationId{"echo.custom"};
    custom.resource = echo.id;
    custom.method = ce::HttpMethod::Get;
    custom.pathTemplate = "/custom-ua";
    custom.expectStatus = 200;
    custom.headers["User-Agent"] = "Custom/9.9";
    echo.operations["custom"] = std::move(custom);

    p.resources[ce::ResourceId{"echo"}] = std::move(echo);
    return p;
}

}  // namespace

class DefaultHeadersFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ =
            std::make_unique<ct::MockSutHarness>(fixturesDir() / "default-headers-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(DefaultHeadersFixture, auto_generated_headers_reach_the_wire) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.plain"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/plain");
    ASSERT_TRUE(cap["found"].get<bool>());
    const auto& headers = cap["headers"];

    ASSERT_TRUE(headers.contains("user-agent"));
    EXPECT_EQ(headers["user-agent"].get<std::string>(), std::string(ce::kDefaultUserAgent));
    EXPECT_TRUE(headers.contains("accept-encoding"));
    EXPECT_TRUE(headers.contains("connection"));
}

TEST_F(DefaultHeadersFixture, caller_user_agent_overrides_the_default) {
    auto project = makeProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"echo.custom"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/custom-ua");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["headers"]["user-agent"].get<std::string>(), "Custom/9.9");
}
