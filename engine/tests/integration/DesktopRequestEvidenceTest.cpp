// DesktopRequestEvidenceTest — owner decision 2026-11-19: a person debugging in the desktop app
// sees what was actually sent and received. For a desktop run (no `RunOptions::agentOutput`) the
// request URL is real and request and response header values are real, `Authorization`,
// `Cookie` and `Set-Cookie` included. Only the values of declared secrets (`{{secret.*}}`, also
// through a `!secret` environment value) stay masked, and a URL's userinfo password. Agent and
// CLI runs stay masked exactly as before. All values are fake.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;

namespace {

constexpr const char* kSessionToken = "fake-session-token-abcdef0001";
constexpr const char* kCookieValue = "fake-cookie-value-0002";
constexpr const char* kApiKey = "fake-declared-secret-0003";
constexpr const char* kEnvSecret = "fake-env-secret-0004";
constexpr const char* kPassword = "fake-admin-password-0005";

class ScriptedHttp final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& req) override {
        ce::HttpResponse r;
        r.status = 200;
        if (req.url.find("/login") != std::string::npos) {
            r.headers = {{"Content-Type", "application/json"},
                         {"Set-Cookie", std::string{"sid="} + kCookieValue + "; Path=/"}};
            r.body = std::string{R"({"token":")"} + kSessionToken + R"("})";
        } else {
            r.headers = {{"Content-Type", "application/json"}};
            r.body = R"({"ok":true})";
        }
        return r;
    }
};

[[nodiscard]] ce::Project project(const std::string& baseUrl = "http://sut.test") {
    ce::Project p;
    p.name = "Evidence";
    p.id = "proj-evidence";
    p.rootDir = std::filesystem::temp_directory_path() / "reqloom-evidence-never-created";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", baseUrl}, {"env_key", "{{secret.ENV_KEY}}"}};

    ce::Actor admin;
    admin.id = ce::ActorId{"admin"};
    admin.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep login;
    login.id = "login";
    login.method = ce::HttpMethod::Post;
    login.pathTemplate = "/login";
    login.bodyTemplate = R"({"user":"admin","password":"{{secret.ADMIN_PASSWORD}}"})";
    login.extractions.push_back(ce::Extraction{.variableName = "token", .sourcePath = "$.token"});
    admin.authSteps.push_back(login);
    admin.inject.headers["Authorization"] = "Bearer {{admin.token}}";
    p.actors[admin.id] = admin;

    ce::Operation op;
    op.id = ce::OperationId{"order.list"};
    op.resource = ce::ResourceId{"order"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/orders?page=1&q=shoes";
    op.actor = admin.id;
    op.headers["X-Key"] = "{{secret.API_KEY}}";
    op.headers["X-Env"] = "{{env.env_key}}";
    op.expectStatus = 200;
    ce::Resource order;
    order.id = op.resource;
    order.operations["list"] = op;
    p.resources[order.id] = order;
    return p;
}

[[nodiscard]] std::vector<ce::RunEvent> run(const ce::Project& p, ce::RunOptions options) {
    ce::ExecutionEngine::Dependencies deps{
        std::make_unique<ScriptedHttp>(),
        nullptr,
        nullptr,
        std::make_unique<reqloom::tests::MemorySecretStore>(
            std::map<std::string, std::string>{{"proj-evidence/API_KEY", kApiKey},
                                               {"proj-evidence/ENV_KEY", kEnvSecret},
                                               {"proj-evidence/ADMIN_PASSWORD", kPassword}}),
        nullptr};
    deps.trust = std::make_unique<reqloom::tests::MemoryTrustStore>();
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(deps)};
    std::vector<ce::RunEvent> events;
    engine.subscribe([&events](const ce::RunEvent& e) { events.push_back(e); });
    ce::RunContext ctx;
    const auto result = engine.run(p, ce::OperationId{"order.list"}, ctx, options);
    EXPECT_TRUE(result.has_value()) << result.error().detail;
    if (result) {
        EXPECT_TRUE(result->succeeded()) << result->steps.back().detail;
    }
    return events;
}

[[nodiscard]] const ce::RequestPrepared* request(const std::vector<ce::RunEvent>& events,
                                                 ce::ExchangeKind kind) {
    for (const auto& e : events) {
        if (const auto* p = std::get_if<ce::RequestPrepared>(&e); p && p->exchange == kind) {
            return p;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string header(const std::vector<std::pair<std::string, std::string>>& hs,
                                 const std::string& name) {
    for (const auto& [k, v] : hs) {
        if (k == name) {
            return v;
        }
    }
    return "<absent>";
}

/// Every URL and header value of every request and response event, joined.
[[nodiscard]] std::string requestEvidence(const std::vector<ce::RunEvent>& events) {
    std::string out;
    for (const auto& e : events) {
        if (const auto* p = std::get_if<ce::RequestPrepared>(&e)) {
            out += p->url + "\n";
            for (const auto& [k, v] : p->maskedHeaders) {
                out += k + ": " + v + "\n";
            }
        } else if (const auto* r = std::get_if<ce::ResponseReceived>(&e)) {
            for (const auto& [k, v] : r->headers) {
                out += k + ": " + v + "\n";
            }
        }
    }
    return out;
}

}  // namespace

TEST(DesktopRequestEvidence, a_desktop_run_shows_the_real_url_and_header_values) {
    const auto events = run(project(), ce::RunOptions{});
    const auto* op = request(events, ce::ExchangeKind::Operation);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->url, "http://sut.test/orders?page=1&q=shoes");
    // A session token from a sign-in response is not a declared secret: the person sees it.
    EXPECT_EQ(header(op->maskedHeaders, "Authorization"), std::string{"Bearer "} + kSessionToken);
    // Declared secrets stay masked, directly and through a `!secret` environment value; the
    // marker names the secret so the person can look its stored value up in the app.
    EXPECT_EQ(header(op->maskedHeaders, "X-Key"), "<secret:API_KEY>");
    EXPECT_EQ(header(op->maskedHeaders, "X-Env"), "<secret:ENV_KEY>");

    const auto* signIn = request(events, ce::ExchangeKind::SignIn);
    ASSERT_NE(signIn, nullptr);
    EXPECT_EQ(signIn->url, "http://sut.test/login");

    bool sawCookie = false;
    for (const auto& e : events) {
        if (const auto* r = std::get_if<ce::ResponseReceived>(&e)) {
            sawCookie = sawCookie ||
                        header(r->headers, "Set-Cookie").find(kCookieValue) != std::string::npos;
        }
    }
    EXPECT_TRUE(sawCookie) << requestEvidence(events);
    const auto all = requestEvidence(events);
    for (const char* secret : {kApiKey, kEnvSecret, kPassword}) {
        EXPECT_EQ(all.find(secret), std::string::npos) << secret << "\n" << all;
    }
}

TEST(DesktopRequestEvidence, a_url_password_stays_masked) {
    const auto events = run(project("http://user:fake-url-pass-0006@sut.test"), ce::RunOptions{});
    const auto* op = request(events, ce::ExchangeKind::Operation);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->url, "http://user:<redacted>@sut.test/orders?page=1&q=shoes");
}

TEST(DesktopRequestEvidence, an_agent_run_stays_masked_as_before) {
    // No secret names either: agent output is unchanged.
    ce::RunOptions cli;
    cli.agentOutput = true;
    cli.recordResponses = true;
    const auto events = run(project(), cli);
    const auto* op = request(events, ce::ExchangeKind::Operation);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->url.find("orders"), std::string::npos) << op->url;
    EXPECT_EQ(header(op->maskedHeaders, "Authorization"), "<redacted>");
    EXPECT_EQ(header(op->maskedHeaders, "X-Key"), "<redacted>");
    const auto all = requestEvidence(events);
    EXPECT_EQ(all.find("<secret:"), std::string::npos) << all;
    for (const char* value : {kSessionToken, kCookieValue, kApiKey, kEnvSecret, kPassword}) {
        EXPECT_EQ(all.find(value), std::string::npos) << value << "\n" << all;
    }
}
