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
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <support/TempPath.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "cookie-jar-project" / "reqloom.yaml");
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
    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
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
    // Login set auth_marker=alice-only. Create set csrf=token-1 (and a
    // session cookie). The jar contract under test here is "cookies
    // absorbed on op A carry forward to op B".
    //
    // NOTE: we intentionally do NOT assert on the session=old-vs-new
    // collision winner here. The mock SUT (cpp-httplib) stores response
    // headers in a std::unordered_multimap, which gives no guarantee about
    // the relative order of two same-name Set-Cookie headers on the wire —
    // and that order differs between libc++ (macOS) and libstdc++ (Linux).
    // The engine's "last wins" (RFC 6265 §5.3) is correct and is covered
    // deterministically by CookiesTest.preserves_emission_order... and the
    // SmokeTest jar last-write-wins case, which feed ordered headers
    // directly without going over the wire.
    EXPECT_NE(cookieHeader.find("auth_marker=alice-only"), std::string::npos)
        << "login-cookie did not carry forward; full Cookie header: " << cookieHeader;
    EXPECT_NE(cookieHeader.find("session="), std::string::npos)
        << "session cookie from create did not carry forward; full Cookie header: " << cookieHeader;
    EXPECT_NE(cookieHeader.find("csrf=token-1"), std::string::npos)
        << "second distinct cookie did not carry forward; full Cookie header: " << cookieHeader;
}

TEST_F(CookieJarFixture, different_actors_have_isolated_cookie_jars) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
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
    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
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

TEST_F(CookieJarFixture, user_set_cookie_survives_401_recovery) {
    // Regression test for an issue where the 401-recovery path in
    // ExecutionEngine erased and re-emitted the Cookie header from the
    // jar, clobbering an explicit `headers.Cookie:` on the operation.
    // The first-attempt path correctly guarded with
    // `if (!req.headers.contains("Cookie"))`; the recovery path did
    // not. Mirror the precedence: user-set Cookie wins on the retry too.
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(reqloom::tests::withApprovedHosts(ce::makeDefaultDependencies()));
    ce::RunContext ctx;

    // Populate alice's jar so there's a real choice between jar and
    // user-set Cookie. Without a populated jar the bug would be
    // invisible — the retry would happen to send `manual=override`
    // because the jar was empty.
    auto created = engine.run(project, ce::OperationId{"widget.create"}, ctx);
    ASSERT_TRUE(created.has_value()) << (created ? "" : created.error().detail);
    ASSERT_TRUE(created->succeeded());

    auto recovered = engine.run(project, ce::OperationId{"manual_cookie.send_with_recovery"}, ctx);
    ASSERT_TRUE(recovered.has_value()) << (recovered ? "" : recovered.error().detail);
    ASSERT_TRUE(recovered->succeeded()) << "401-recovery did not produce a successful retry";

    // The mock SUT served 401 then 200 — confirm we got the second
    // response (post-recovery payload).
    bool sawTwoAttempts = false;
    for (const auto& step : recovered->steps) {
        if (step.op.value == "manual_cookie.send_with_recovery" && !step.pollAttempt) {
            EXPECT_EQ(step.attempts, 2);
            sawTwoAttempts = true;
        }
    }
    EXPECT_TRUE(sawTwoAttempts);

    // The retry's Cookie header must still be exactly `manual=override`.
    // Pre-fix, the recovery code erased Cookie and emitted from the jar
    // (which contained alice's auth_marker, session, csrf...).
    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/manual-401");
    ASSERT_TRUE(cap["found"].get<bool>());
    const std::string cookieHeader = cap["headers"]["cookie"].get<std::string>();
    EXPECT_EQ(cookieHeader, "manual=override")
        << "401-recovery clobbered user-set Cookie with jar contents; full Cookie: "
        << cookieHeader;
}

// ─── Cookie scope: host, Domain, Secure, Path, removal ──────────────────────

namespace {

/// Answers 200 by URL path; `/set` and `/logout` carry the Set-Cookie headers a test
/// configures. Records every request.
class CookieHttpClient final : public ce::HttpClient {
public:
    std::map<std::string, std::vector<std::string>> setCookies;

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests.push_back(request);
        ce::HttpResponse response;
        response.status = 200;
        response.body = "{}";
        const auto path = request.url.substr(request.url.find('/', request.url.find("://") + 3));
        if (const auto it = setCookies.find(path); it != setCookies.end()) {
            for (const auto& header : it->second) {
                response.headers.emplace_back("Set-Cookie", header);
            }
        }
        return response;
    }

    std::vector<ce::HttpRequest> requests;
};

constexpr const char* kCookieScopeYaml = R"YAML(
version: 1
name: CookieScope
default_environment: api
actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /login
      expect_status: 200
resources:
  w:
    operations:
      set: { method: GET, path: /set, actor: user, expect_status: 200 }
      logout: { method: GET, path: /logout, actor: user, expect_status: 200 }
      read: { method: GET, path: /read, actor: user, expect_status: 200 }
      other: { method: GET, path: /other, actor: user, expect_status: 200 }
)YAML";

/// One engine and run context shared by every run of a test, so the jar carries over.
class CookieScopeRig {
public:
    explicit CookieScopeRig(std::map<std::string, std::vector<std::string>> setCookies) {
        dir_ = ct::uniqueTempPath("reqloom-cookie-scope");
        fs::create_directories(dir_);
        std::ofstream{dir_ / "reqloom.yaml"} << kCookieScopeYaml;
        auto parsed = ce::parseProject(dir_ / "reqloom.yaml");
        EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().detail);
        if (parsed) {
            project_ = std::move(*parsed);
        }
        project_.environments["api"] = {{"baseUrl", "http://api.example.test"}};
        project_.environments["www"] = {{"baseUrl", "http://www.example.test"}};
        project_.environments["other"] = {{"baseUrl", "http://other.test"}};
        project_.environments["tls"] = {{"baseUrl", "https://api.example.test"}};
        auto http = std::make_unique<CookieHttpClient>();
        http->setCookies = std::move(setCookies);
        http_ = http.get();
        engine_ = std::make_unique<ce::ExecutionEngine>(
            reqloom::tests::withApprovedHosts(ce::ExecutionEngine::Dependencies{
                std::move(http), nullptr, nullptr, nullptr, nullptr}));
    }
    ~CookieScopeRig() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    CookieScopeRig(const CookieScopeRig&) = delete;
    CookieScopeRig& operator=(const CookieScopeRig&) = delete;
    CookieScopeRig(CookieScopeRig&&) = delete;
    CookieScopeRig& operator=(CookieScopeRig&&) = delete;

    /// Run `op` against `env` and return the Cookie header its request carried.
    std::optional<std::string> run(const std::string& op, const std::string& env) {
        ce::RunOptions options;
        options.environment = env;
        const auto before = http_->requests.size();
        auto result = engine_->run(project_, ce::OperationId{op}, ctx_, options);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
        EXPECT_TRUE(result && result->succeeded()) << op << " on " << env;
        for (auto i = http_->requests.size(); i > before; --i) {
            const auto& request = http_->requests[i - 1];
            if (request.url.ends_with(op.substr(op.find('.') + 1))) {
                const auto it = request.headers.find("Cookie");
                return it == request.headers.end() ? std::nullopt
                                                   : std::optional<std::string>{it->second};
            }
        }
        ADD_FAILURE() << op << " sent no request";
        return std::nullopt;
    }

    [[nodiscard]] const ce::RunContext& context() const { return ctx_; }

private:
    fs::path dir_;
    ce::Project project_;
    CookieHttpClient* http_{nullptr};
    std::unique_ptr<ce::ExecutionEngine> engine_;
    ce::RunContext ctx_;
};

}  // namespace

TEST(CookieScope, cookie_set_by_one_host_is_not_sent_to_another) {
    CookieScopeRig rig{{{"/set", {"sid=s-1"}}}};
    rig.run("w.set", "api");
    EXPECT_EQ(rig.run("w.read", "api"), "sid=s-1") << "the setting host still gets it";
    EXPECT_EQ(rig.run("w.read", "other"), std::nullopt);
    EXPECT_EQ(rig.run("w.read", "www"), std::nullopt) << "host-only: not to a sibling";
}

TEST(CookieScope, domain_cookie_is_sent_to_a_subdomain) {
    CookieScopeRig rig{{{"/set", {"sid=s-1; Domain=example.test"}}}};
    rig.run("w.set", "api");
    EXPECT_EQ(rig.run("w.read", "www"), "sid=s-1");
    EXPECT_EQ(rig.run("w.read", "other"), std::nullopt);
}

TEST(CookieScope, secure_cookie_is_not_sent_over_http) {
    CookieScopeRig rig{{{"/set", {"sid=s-1; Secure"}}}};
    rig.run("w.set", "tls");
    EXPECT_EQ(rig.run("w.read", "tls"), "sid=s-1");
    EXPECT_EQ(rig.run("w.read", "api"), std::nullopt);
}

TEST(CookieScope, max_age_zero_on_an_ordinary_response_removes_the_cookie) {
    CookieScopeRig rig{{{"/set", {"sid=s-1", "keep=k-1"}}, {"/logout", {"sid=gone; Max-Age=0"}}}};
    rig.run("w.set", "api");
    rig.run("w.logout", "api");
    EXPECT_EQ(rig.run("w.read", "api"), "keep=k-1");
    EXPECT_FALSE(rig.context().cookies(ce::ActorId{"user"}).contains("sid"));
}

TEST(CookieScope, path_attribute_limits_where_the_cookie_goes) {
    CookieScopeRig rig{{{"/set", {"sid=s-1; Path=/read"}}}};
    rig.run("w.set", "api");
    EXPECT_EQ(rig.run("w.read", "api"), "sid=s-1");
    EXPECT_EQ(rig.run("w.other", "api"), std::nullopt);
}

TEST(CookieScope, jar_records_the_host_that_set_each_cookie) {
    CookieScopeRig rig{{{"/set", {"sid=s-1; Domain=example.test; Path=/; Secure"}}}};
    rig.run("w.set", "tls");
    const auto jar = rig.context().jarCookies(ce::ActorId{"user"});
    ASSERT_EQ(jar.size(), 1U);
    EXPECT_EQ(jar.front().name, "sid");
    EXPECT_EQ(jar.front().value, "s-1");
    EXPECT_EQ(jar.front().setterHost, "api.example.test");
    EXPECT_EQ(jar.front().domain, "example.test");
    EXPECT_EQ(jar.front().path, "/");
    EXPECT_TRUE(jar.front().secure);
    EXPECT_EQ(rig.context().cookies(ce::ActorId{"user"}).at("sid"), "s-1");
}
