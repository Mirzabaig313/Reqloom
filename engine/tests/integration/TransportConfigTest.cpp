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

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace ce = chainapi::engine;

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
