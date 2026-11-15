// TransportConfigTest — confirms the per-environment transport block
// actually reaches libcurl. The decisive test is the connect-timeout
// knob: when set very low, a request to a non-routable address must
// fail in well under the default 5-second budget. If the knob is
// silently dropped on the way through ResolveContext → HttpRequest →
// CurlHttpClient, the test fails because the engine waits the full
// libcurl default.
//
// Why connect timeout and not TLS or proxy? TLS verification needs a
// real HTTPS server (or a stub with self-signed certs) which inflates
// the test harness. Proxy routing needs an actual HTTP-CONNECT-aware
// peer, which the mock-sut isn't. Connect timeout is plumbed through
// the same `req.transport` path either of those would use — confirming
// it works confirms the wiring is intact.

#include "MockSutHarness.h"
#include "infrastructure/http/CurlHttpClient.h"
#include "support/TempPath.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace ce = reqloom::engine;

namespace {

ce::Project buildProject(std::chrono::milliseconds connectTimeout) {
    ce::Project p;
    p.name = "TransportConnectTimeout";
    p.defaultEnvironment = "local";

    // 198.51.100.1 is RFC 5737 TEST-NET-2 — guaranteed non-routable on
    // the public internet, and on most LANs nothing answers there. The
    // TCP connect attempt must hit the configured timeout.
    p.environments["local"] = {{"baseUrl", "http://198.51.100.1:9"}};

    ce::TransportConfig t;
    t.connectTimeout = connectTimeout;
    p.transport["local"] = std::move(t);

    ce::Actor user;
    user.id = ce::ActorId{"user"};
    user.strategy = ce::AuthStrategy::Simple;

    ce::AuthStep login;
    login.id = "login";
    login.method = ce::HttpMethod::Post;
    login.pathTemplate = "/login";
    login.bodyTemplate = R"({})";
    login.expectStatus = 200;
    login.extractions.push_back({"token", "$.token", ce::Extraction::Source::JsonPath});
    user.authSteps.push_back(std::move(login));

    const auto userId = user.id;  // capture before std::move below
    p.actors[userId] = std::move(user);

    ce::Resource ping;
    ping.id = ce::ResourceId{"ping"};
    ce::Operation getOp;
    getOp.id = ce::OperationId{"ping.get"};
    getOp.resource = ping.id;
    getOp.actor = userId;
    getOp.method = ce::HttpMethod::Get;
    getOp.pathTemplate = "/ping";
    getOp.expectStatus = 200;
    ping.operations["get"] = std::move(getOp);
    p.resources[ping.id] = std::move(ping);
    return p;
}

}  // namespace

TEST(TransportConfigIntegration, low_connect_timeout_aborts_quickly) {
    // 200ms is well below libcurl's 5-second default. If our knob
    // reaches CURLOPT_CONNECTTIMEOUT_MS, the run must fail in around
    // that timeframe rather than the default 5s.
    auto project = buildProject(std::chrono::milliseconds{200});

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    const auto t0 = std::chrono::steady_clock::now();
    auto result = engine.run(project, ce::OperationId{"ping.get"}, ctx);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // The engine returns Ok with a Failed RunResult — auth flow
    // network-errored against the non-routable destination.
    if (result.has_value()) {
        EXPECT_FALSE(result->succeeded());
    }

    // The auth step doesn't retry on network errors; one send with a
    // 200ms connect timeout should complete in well under a second on
    // real hardware. Allow generous headroom for CI runners and
    // sanitizer overhead — 2s is the soft cap. Anything closer to the
    // libcurl default of 5s indicates the knob isn't reaching curl.
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(elapsedMs, 2000) << "connect_timeout knob did not reach libcurl — request waited "
                               << elapsedMs << "ms"
                               << " (libcurl default is 5000ms)";
}

// ─── CurlHttpClient transport behaviour against the mock SUT ────────────────
// These drive CurlHttpClient directly (no engine) so each assertion pins
// one transport property: HEAD/OPTIONS method handling, which response's
// headers survive a redirect chain, allowed URL schemes, the response
// body cap and the redirect limit.

namespace {

[[nodiscard]] bool hasHeader(const ce::HttpResponse& r, std::string_view name) {
    return std::ranges::any_of(r.headers, [name](const auto& kv) {
        return std::ranges::equal(kv.first, name, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    });
}

}  // namespace

class CurlTransportFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<reqloom::tests::MockSutHarness>(
            std::filesystem::path(REQLOOM_FIXTURES_DIR) / "transport-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    [[nodiscard]] ce::HttpRequest makeRequest(ce::HttpMethod method,
                                              const std::string& path) const {
        ce::HttpRequest r;
        r.method = method;
        r.url = harness_->baseUrl() + path;
        // Short budget so a hang (e.g. HEAD waiting for a body) fails fast.
        r.timeout = std::chrono::milliseconds{2'000};
        return r;
    }

    std::unique_ptr<reqloom::tests::MockSutHarness> harness_;
    ce::CurlHttpClient client_;
};

TEST_F(CurlTransportFixture, head_request_does_not_wait_for_a_body) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Head, "/resource"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(res->body.empty());
    EXPECT_TRUE(hasHeader(*res, "Content-Length"));
}

TEST_F(CurlTransportFixture, options_request_is_sent_as_options) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Options, "/resource"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(hasHeader(*res, "Allow"));
}

TEST_F(CurlTransportFixture, redirect_keeps_only_the_final_responses_headers) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Get, "/hop1"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 200);
    EXPECT_TRUE(hasHeader(*res, "X-Final"));
    EXPECT_FALSE(hasHeader(*res, "X-Hop-Only")) << "header from the 302 hop leaked through";
    EXPECT_FALSE(hasHeader(*res, "Location")) << "header from the 302 hop leaked through";
}

TEST_F(CurlTransportFixture, file_scheme_is_refused_without_reading_the_file) {
    const auto path = reqloom::tests::uniqueTempPath("reqloom-file-scheme", ".txt");
    {
        std::ofstream out(path);
        out << "TOP-SECRET-CONTENTS";
    }
    ce::HttpRequest r;
    r.url = "file://" + path.generic_string();
    r.timeout = std::chrono::milliseconds{2'000};
    auto res = client_.send(r);
    std::filesystem::remove(path);

    ASSERT_FALSE(res.has_value()) << "file:// URL was fetched; body: " << res->body;
    EXPECT_EQ(res.error().code, ce::ErrorCode::NetworkPolicy);
    EXPECT_NE(res.error().detail.find("http"), std::string::npos) << res.error().detail;
}

TEST_F(CurlTransportFixture, body_within_default_cap_is_returned) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Get, "/big"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->body.size(), 100U);
}

TEST_F(CurlTransportFixture, body_larger_than_configured_cap_fails) {
    auto req = makeRequest(ce::HttpMethod::Get, "/big");
    req.transport.maxResponseBytes = 16;
    auto res = client_.send(req);
    ASSERT_FALSE(res.has_value()) << "body of " << res->body.size() << " bytes was accepted";
    EXPECT_EQ(res.error().code, ce::ErrorCode::NetworkPolicy);
    EXPECT_NE(res.error().detail.find("16 bytes"), std::string::npos) << res.error().detail;
}

TEST_F(CurlTransportFixture, ten_redirects_are_followed) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Get, "/ten"));
    ASSERT_TRUE(res.has_value()) << res.error().detail;
    EXPECT_EQ(res->status, 200);
}

TEST_F(CurlTransportFixture, eleventh_redirect_is_refused) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Get, "/eleven"));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ce::ErrorCode::NetworkPolicy);
    EXPECT_NE(res.error().detail.find("redirect"), std::string::npos) << res.error().detail;
}

TEST_F(CurlTransportFixture, redirect_loop_stops_at_the_limit) {
    auto res = client_.send(makeRequest(ce::HttpMethod::Get, "/loop"));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ce::ErrorCode::NetworkPolicy);
}
