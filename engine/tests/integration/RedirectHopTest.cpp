// RedirectHopTest — what CurlHttpClient sends on each redirect hop: method, body and which headers
// reach the next host. Pins the behaviour the hop-by-hop redirect loop must keep, and the hook
// (`HttpRequest::onRedirect`) the engine uses to check host approval on every hop (FR-15.9).
#include "MockSutHarness.h"
#include "infrastructure/http/CurlHttpClient.h"
#include "support/TempPath.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
using nlohmann::json;

namespace {

[[nodiscard]] std::filesystem::path writeRoutes(const json& routes) {
    const auto path = ct::uniqueTempPath("reqloom-redirect-routes", ".json");
    std::ofstream out{path};
    out << json{{"routes", routes}}.dump();
    return path;
}

[[nodiscard]] json landingRoutes() {
    json routes = json::array();
    for (const char* method : {"GET", "POST", "PUT"}) {
        routes.push_back(
            {{"method", method}, {"path", "/landing"}, {"status", 200}, {"body", "ok"}});
    }
    return routes;
}

[[nodiscard]] json redirect(const char* method,
                            const std::string& path,
                            int status,
                            const std::string& location) {
    return {{"method", method},
            {"path", path},
            {"status", status},
            {"headers", {{"Location", location}}}};
}

/// What the landing route received last, via the mock's inspection endpoint.
[[nodiscard]] json lastRequest(ce::CurlHttpClient& client, const std::string& baseUrl) {
    ce::HttpRequest probe;
    probe.url = baseUrl + "/__mock/last-request?path=/landing";
    auto res = client.send(probe);
    if (!res) {
        return json::object();
    }
    return json::parse(res->body, nullptr, false);
}

class RedirectHopTest : public ::testing::Test {
protected:
    void SetUp() override {
        landingRoutesFile_ = writeRoutes(landingRoutes());
        target_ = std::make_unique<ct::MockSutHarness>(landingRoutesFile_);
        const auto cross = target_->baseUrl() + "/landing";
        json routes = landingRoutes();
        for (const char* method : {"GET", "POST", "PUT"}) {
            routes.push_back(redirect(method, "/same-302", 302, "/landing"));
            routes.push_back(redirect(method, "/same-303", 303, "/landing"));
            routes.push_back(redirect(method, "/same-307", 307, "/landing"));
            routes.push_back(redirect(method, "/cross-302", 302, cross));
            routes.push_back(redirect(method, "/cross-307", 307, cross));
        }
        originRoutesFile_ = writeRoutes(routes);
        origin_ = std::make_unique<ct::MockSutHarness>(originRoutesFile_);
    }

    void TearDown() override {
        origin_.reset();
        target_.reset();
        std::filesystem::remove(landingRoutesFile_);
        std::filesystem::remove(originRoutesFile_);
    }

    [[nodiscard]] ce::HttpRequest post(const std::string& path) const {
        ce::HttpRequest r;
        r.method = ce::HttpMethod::Post;
        r.url = origin_->baseUrl() + path;
        r.body = R"({"note":"hello"})";
        r.headers["Content-Type"] = "application/json";
        r.timeout = std::chrono::milliseconds{5'000};
        return r;
    }

    std::filesystem::path landingRoutesFile_;
    std::filesystem::path originRoutesFile_;
    std::unique_ptr<ct::MockSutHarness> target_;
    std::unique_ptr<ct::MockSutHarness> origin_;
    ce::CurlHttpClient client_;
};

}  // namespace

// libcurl with CURLOPT_CUSTOMREQUEST keeps the method string on a 302/303 and drops the body; the
// hop-by-hop loop keeps that behaviour rather than change what existing projects send.
TEST_F(RedirectHopTest, post_302_keeps_the_method_and_drops_the_body) {
    auto res = client_.send(post("/same-302"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 200);
    const auto seen = lastRequest(client_, origin_->baseUrl());
    EXPECT_EQ(seen.value("method", ""), "POST");
    EXPECT_EQ(seen.value("raw_body", "?"), "");
}

TEST_F(RedirectHopTest, post_303_keeps_the_method_and_drops_the_body) {
    auto res = client_.send(post("/same-303"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto seen = lastRequest(client_, origin_->baseUrl());
    EXPECT_EQ(seen.value("method", ""), "POST");
    EXPECT_EQ(seen.value("raw_body", "?"), "");
}

TEST_F(RedirectHopTest, post_307_keeps_method_and_body) {
    auto res = client_.send(post("/same-307"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto seen = lastRequest(client_, origin_->baseUrl());
    EXPECT_EQ(seen.value("method", ""), "POST");
    EXPECT_EQ(seen.value("raw_body", ""), R"({"note":"hello"})");
}

TEST_F(RedirectHopTest, put_302_keeps_the_method) {
    auto req = post("/same-302");
    req.method = ce::HttpMethod::Put;
    auto res = client_.send(req);
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto seen = lastRequest(client_, origin_->baseUrl());
    EXPECT_EQ(seen.value("method", ""), "PUT");
}

TEST_F(RedirectHopTest, same_host_redirect_keeps_every_header) {
    auto req = post("/same-307");
    req.headers["Authorization"] = "Bearer fake-test-token-0001";
    req.headers["X-Api-Key"] = "fake-test-key-0002";
    auto res = client_.send(req);
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto headers = lastRequest(client_, origin_->baseUrl())["headers"];
    EXPECT_EQ(headers.value("authorization", ""), "Bearer fake-test-token-0001");
    EXPECT_EQ(headers.value("x-api-key", ""), "fake-test-key-0002");
}

TEST_F(RedirectHopTest, cross_host_redirect_drops_authorization_and_cookie) {
    auto req = post("/cross-302");
    req.method = ce::HttpMethod::Get;
    req.body.reset();
    req.headers["Authorization"] = "Bearer fake-test-token-0001";
    req.headers["Cookie"] = "session=fake-test-cookie-0003";
    auto res = client_.send(req);
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto headers = lastRequest(client_, target_->baseUrl())["headers"];
    EXPECT_FALSE(headers.contains("authorization")) << headers.dump();
    EXPECT_FALSE(headers.contains("cookie")) << headers.dump();
}

TEST_F(RedirectHopTest, on_redirect_sees_each_hop_and_can_refuse_it) {
    auto req = post("/cross-307");
    std::vector<std::string> hops;
    req.onRedirect = [&hops](ce::HttpRequest& next) -> std::expected<void, ce::ReqloomError> {
        hops.push_back(next.url);
        return std::unexpected(ce::ReqloomError{
            ce::ErrorCode::HostNotApproved, ce::ErrorClass::Auth, "refused by the test"});
    };
    auto res = client_.send(req);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ce::ErrorCode::HostNotApproved);
    ASSERT_EQ(hops.size(), 1U);
    EXPECT_EQ(hops.front(), target_->baseUrl() + "/landing");
    EXPECT_FALSE(lastRequest(client_, target_->baseUrl()).value("found", true))
        << "the refused hop reached the next host";
}

TEST_F(RedirectHopTest, on_redirect_can_remove_a_header_before_the_hop) {
    auto req = post("/cross-307");
    req.headers["X-Api-Key"] = "fake-test-key-0002";
    req.onRedirect = [](ce::HttpRequest& next) -> std::expected<void, ce::ReqloomError> {
        next.headers.erase("X-Api-Key");
        return {};
    };
    auto res = client_.send(req);
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    const auto seen = lastRequest(client_, target_->baseUrl());
    EXPECT_EQ(seen.value("method", ""), "POST");
    EXPECT_FALSE(seen["headers"].contains("x-api-key")) << seen.dump();
}

// Review finding 6: a timeout of 0 means "no limit" to libcurl, so it must not end a redirect.
TEST_F(RedirectHopTest, zero_timeout_means_no_limit_across_hops) {
    auto req = post("/same-307");
    req.timeout = std::chrono::milliseconds{0};
    auto res = client_.send(req);
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 200);
}
