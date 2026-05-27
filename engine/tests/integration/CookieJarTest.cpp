// CookieJarTest — confirms cookies absorbed from a response on op A
// reach op B's outbound `Cookie:` header automatically, and that
// per-actor isolation holds.
//
// Each test fails on the parent commit:
//   - Cookies were resolved at extraction time only, not stored.
//   - Cross-operation jar didn't exist; subsequent ops sent no Cookie
//     header even when the prior response set one.
//   - Multi Set-Cookie collisions in one response were resolved
//     "first wins"; the new behavior is "last wins" per RFC 6265 §5.3.

#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "cookie-jar-project" / "chainapi.yaml");
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

class CookieJarFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "cookie-jar-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(CookieJarFixture, second_op_carries_cookies_set_by_first_op) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto created = engine.run(project, ce::OperationId{"widget.create"}, ctx);
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().detail);
    ASSERT_TRUE(created->succeeded());

    auto read = engine.run(project, ce::OperationId{"widget.read"}, ctx);
    ASSERT_TRUE(read.has_value()) << (read ? "" : read.error().detail);
    ASSERT_TRUE(read->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/widgets/wid-1");
    ASSERT_TRUE(cap["found"].get<bool>());

    const std::string cookieHeader = cap["headers"]["cookie"].get<std::string>();
    // Login set auth_marker=alice-only. Create set session=old then
    // session=new (last wins) plus csrf=token-1. All four key-value
    // checks below must hold for the jar contract to be intact.
    EXPECT_NE(cookieHeader.find("auth_marker=alice-only"), std::string::npos)
        << "login-cookie did not carry forward; full Cookie header: " << cookieHeader;
    EXPECT_NE(cookieHeader.find("session=new"), std::string::npos)
        << "multi-cookie collision should leave LAST value (session=new) in jar; full Cookie "
           "header: "
        << cookieHeader;
    EXPECT_EQ(cookieHeader.find("session=old"), std::string::npos)
        << "earlier (overridden) cookie value must not appear; full Cookie header: "
        << cookieHeader;
    EXPECT_NE(cookieHeader.find("csrf=token-1"), std::string::npos)
        << "second distinct cookie did not carry forward; full Cookie header: " << cookieHeader;
}

TEST_F(CookieJarFixture, different_actors_have_isolated_cookie_jars) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    // Run as alice — populates alice's jar with auth_marker.
    auto created = engine.run(project, ce::OperationId{"widget.create"}, ctx);
    ASSERT_TRUE(created.has_value());
    ASSERT_TRUE(created->succeeded());

    // Now run as bob. Bob's jar must NOT carry alice's cookies.
    auto bobRead = engine.run(project, ce::OperationId{"widget.read_as_bob"}, ctx);
    ASSERT_TRUE(bobRead.has_value()) << (bobRead ? "" : bobRead.error().detail);
    ASSERT_TRUE(bobRead->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/widgets/bob-only");
    ASSERT_TRUE(cap["found"].get<bool>());

    if (cap["headers"].contains("cookie")) {
        const std::string cookieHeader = cap["headers"]["cookie"].get<std::string>();
        EXPECT_EQ(cookieHeader.find("auth_marker=alice-only"), std::string::npos)
            << "alice's auth marker leaked into bob's request; full Cookie header: "
            << cookieHeader;
        EXPECT_EQ(cookieHeader.find("session="), std::string::npos)
            << "alice's session cookie leaked into bob's request; full Cookie header: "
            << cookieHeader;
    }
}

TEST_F(CookieJarFixture, user_set_cookie_header_wins_over_jar) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    // Populate the jar first.
    auto created = engine.run(project, ce::OperationId{"widget.create"}, ctx);
    ASSERT_TRUE(created.has_value());
    ASSERT_TRUE(created->succeeded());

    // Then run an op that sets `Cookie: manual=override` explicitly.
    // The jar must not be merged in — explicit user header wins.
    auto manual = engine.run(project, ce::OperationId{"manual_cookie.send"}, ctx);
    ASSERT_TRUE(manual.has_value()) << (manual ? "" : manual.error().detail);
    ASSERT_TRUE(manual->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/manual");
    ASSERT_TRUE(cap["found"].get<bool>());
    const std::string cookieHeader = cap["headers"]["cookie"].get<std::string>();
    EXPECT_EQ(cookieHeader, "manual=override")
        << "user-set Cookie header should be sent verbatim; full Cookie header: " << cookieHeader;
}
