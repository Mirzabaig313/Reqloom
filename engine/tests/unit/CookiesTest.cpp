// Cookies — unit tests for the small parsing helpers in
// engine/src/application/Cookies.h.
//
// Each test pins one corner of the contract — Set-Cookie parsing,
// collision rules, and request-header formatting. The integration
// tests in CookieJarTest.cpp prove the executor wires this all the
// way through to the wire.

#include "application/Cookies.h"

#include "domain/SecretRegistry.h"

#include <reqloom/engine/RunContext.h>

#include <gtest/gtest.h>

#include <chrono>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;

// ─── parseSetCookie ─────────────────────────────────────────────────────────

TEST(Cookies, parses_simple_name_value) {
    auto parsed = ce::cookies::parseSetCookie("session=abc-123");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "session");
    EXPECT_EQ(parsed->second, "abc-123");
}

TEST(Cookies, strips_attributes_after_first_semicolon) {
    auto parsed = ce::cookies::parseSetCookie("auth=v1.token; Path=/; HttpOnly; Max-Age=3600");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "auth");
    EXPECT_EQ(parsed->second, "v1.token");
}

TEST(Cookies, trims_surrounding_whitespace) {
    auto parsed = ce::cookies::parseSetCookie("  spaced  =  value  ; Path=/");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "spaced");
    EXPECT_EQ(parsed->second, "value");
}

TEST(Cookies, returns_nullopt_when_no_equals) {
    auto parsed = ce::cookies::parseSetCookie("just-flag; Path=/");
    EXPECT_FALSE(parsed.has_value());
}

TEST(Cookies, returns_nullopt_when_name_empty) {
    auto parsed = ce::cookies::parseSetCookie("=value");
    EXPECT_FALSE(parsed.has_value());
}

TEST(Cookies, accepts_empty_value) {
    auto parsed = ce::cookies::parseSetCookie("logout=; Path=/; Max-Age=0");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "logout");
    EXPECT_TRUE(parsed->second.empty());
}

// ─── collectFromResponse ────────────────────────────────────────────────────

TEST(Cookies, collects_only_set_cookie_headers_case_insensitively) {
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Set-Cookie", "a=1"},
        {"set-cookie", "b=2"},  // lowercase header name
        {"X-Other", "ignored"},
    };

    const auto cookies = ce::cookies::collectFromResponse(headers);
    ASSERT_EQ(cookies.size(), 2u);
    EXPECT_EQ(cookies[0].first, "a");
    EXPECT_EQ(cookies[1].first, "b");
}

TEST(Cookies, preserves_emission_order_so_callers_can_pick_collision_winner) {
    // Two Set-Cookie entries with the same name — the order is the
    // server's order. Last-wins is the conventional pick (RFC 6265
    // §5.3 step 11) and the executor's findCookie / cookie-jar
    // absorption both rely on this preservation.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Set-Cookie", "session=old; Path=/; Max-Age=0"},
        {"Set-Cookie", "session=new; Path=/"},
    };

    const auto cookies = ce::cookies::collectFromResponse(headers);
    ASSERT_EQ(cookies.size(), 2u);
    EXPECT_EQ(cookies[0].second, "old");
    EXPECT_EQ(cookies[1].second, "new");
}

// ─── formatRequestHeader ───────────────────────────────────────────────────

TEST(Cookies, request_header_is_semicolon_space_separated) {
    const std::map<std::string, std::string> jar = {
        {"a", "1"},
        {"b", "2"},
    };
    const auto header = ce::cookies::formatRequestHeader(jar);
    // std::map iterates in sorted order; the resulting string is
    // deterministic. RFC 6265 §5.4 says order should not be relied
    // upon, so any stable order is acceptable.
    EXPECT_EQ(header, "a=1; b=2");
}

TEST(Cookies, request_header_is_empty_when_jar_is_empty) {
    EXPECT_EQ(ce::cookies::formatRequestHeader({}), "");
}

TEST(Cookies, request_header_emits_each_cookie_with_explicit_equals) {
    const std::map<std::string, std::string> jar = {
        {"empty_value", ""},
        {"normal", "ok"},
    };
    const auto header = ce::cookies::formatRequestHeader(jar);
    // Even an empty value still emits `name=`. Servers that rely on a
    // bare cookie name (uncommon) are out of scope.
    EXPECT_EQ(header, "empty_value=; normal=ok");
}

// ─── Set-Cookie attributes, removal, scope ─────────────────────────────────

namespace {

const auto kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 15};

ce::cookies::SetCookie parsed(std::string_view header) {
    auto cookie = ce::cookies::parseSetCookieHeader(header);
    EXPECT_TRUE(cookie.has_value()) << header;
    return cookie.value_or(ce::cookies::SetCookie{});
}

}  // namespace

TEST(Cookies, parses_attributes_case_insensitively) {
    const auto c = parsed("sid=abc; Domain=.Example.COM; PATH=/api; secure; HttpOnly; Max-Age=60");
    EXPECT_EQ(c.name, "sid");
    EXPECT_EQ(c.value, "abc");
    EXPECT_EQ(c.domain.value_or(""), "example.com");
    EXPECT_EQ(c.path.value_or(""), "/api");
    EXPECT_TRUE(c.secure);
    EXPECT_EQ(c.maxAge.value_or(0), 60);
}

TEST(Cookies, max_age_zero_or_negative_removes_the_cookie) {
    EXPECT_TRUE(parsed("sid=x1; Max-Age=0").removes(kNow));
    EXPECT_TRUE(parsed("sid=x1; Max-Age=-1").removes(kNow));
    EXPECT_FALSE(parsed("sid=x1; Max-Age=60").removes(kNow));
}

TEST(Cookies, expires_in_the_past_removes_the_cookie) {
    // RFC 6265 §5.1.1 date forms: IMF-fixdate, RFC 850 and asctime.
    EXPECT_TRUE(parsed("sid=x1; Expires=Thu, 01 Jan 1970 00:00:00 GMT").removes(kNow));
    EXPECT_TRUE(parsed("sid=x1; Expires=Sunday, 06-Nov-94 08:49:37 GMT").removes(kNow));
    EXPECT_TRUE(parsed("sid=x1; Expires=Sun Nov  6 08:49:37 1994").removes(kNow));
    EXPECT_FALSE(parsed("sid=x1; Expires=Fri, 01 Jan 2100 00:00:00 GMT").removes(kNow));
    // Max-Age wins over Expires (RFC 6265 §5.3 step 3).
    EXPECT_FALSE(parsed("sid=x1; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=60").removes(kNow));
    // An unparseable date is ignored.
    EXPECT_FALSE(parsed("sid=x1; Expires=whenever").removes(kNow));
}

TEST(Cookies, empty_value_removes_the_cookie) {
    EXPECT_TRUE(parsed("sid=; Path=/").removes(kNow));
}

TEST(Cookies, absorbing_a_logout_response_removes_the_jar_entry) {
    ce::RunContext ctx;
    const ce::ActorId actor{"user"};
    ctx.setCookie(actor, "sid", "session-1");
    ctx.setCookie(actor, "keep", "k-1");
    ce::cookies::absorbSetCookies({{"Set-Cookie", "sid=; Max-Age=0"}, {"Set-Cookie", "fresh=f-1"}},
                                  actor,
                                  ctx,
                                  "https://api.example.com/logout");
    const auto jar = ctx.cookies(actor);
    EXPECT_FALSE(jar.contains("sid"));
    EXPECT_EQ(jar.at("keep"), "k-1");
    EXPECT_EQ(jar.at("fresh"), "f-1");
    EXPECT_EQ(ce::cookies::formatRequestHeader(jar), "fresh=f-1; keep=k-1");
}

TEST(Cookies, domain_match_is_exact_or_a_dot_boundary_suffix) {
    EXPECT_TRUE(ce::cookies::domainMatches("example.com", "example.com"));
    EXPECT_TRUE(ce::cookies::domainMatches("api.example.com", "example.com"));
    EXPECT_FALSE(ce::cookies::domainMatches("badexample.com", "example.com"));
    EXPECT_FALSE(ce::cookies::domainMatches("example.com", "api.example.com"));
    EXPECT_FALSE(ce::cookies::domainMatches("10.0.0.1", "0.0.1"));
}

TEST(Cookies, host_only_cookie_is_sent_only_to_the_host_that_set_it) {
    const auto c = parsed("sid=abc123");
    EXPECT_TRUE(ce::cookies::shouldSend(c, "a.example.com", "https://a.example.com/x"));
    EXPECT_FALSE(ce::cookies::shouldSend(c, "a.example.com", "https://b.example.com/x"));
    EXPECT_FALSE(ce::cookies::shouldSend(c, "a.example.com", "https://evil.test/x"));
}

TEST(Cookies, domain_cookie_reaches_subdomains_only_when_the_setter_may_claim_it) {
    const auto parent = parsed("sid=abc123; Domain=example.com");
    EXPECT_TRUE(ce::cookies::shouldSend(parent, "a.example.com", "https://b.example.com/"));
    EXPECT_FALSE(ce::cookies::shouldSend(parent, "a.example.com", "https://example.org/"));
    // A host cannot set a cookie for an unrelated or a child domain.
    const auto foreign = parsed("sid=abc123; Domain=evil.test");
    EXPECT_FALSE(ce::cookies::shouldSend(foreign, "a.example.com", "https://evil.test/"));
    EXPECT_FALSE(ce::cookies::shouldSend(foreign, "a.example.com", "https://a.example.com/"));
}

TEST(Cookies, secure_cookie_is_never_sent_over_http) {
    const auto c = parsed("sid=abc123; Secure");
    EXPECT_TRUE(ce::cookies::shouldSend(c, "example.com", "https://example.com/"));
    EXPECT_FALSE(ce::cookies::shouldSend(c, "example.com", "http://example.com/"));
}

TEST(Cookies, path_attribute_limits_the_request_paths) {
    const auto c = parsed("sid=abc123; Path=/api");
    EXPECT_TRUE(ce::cookies::shouldSend(c, "example.com", "https://example.com/api"));
    EXPECT_TRUE(ce::cookies::shouldSend(c, "example.com", "https://example.com/api/v1?x=1"));
    EXPECT_FALSE(ce::cookies::shouldSend(c, "example.com", "https://example.com/apiary"));
    EXPECT_FALSE(ce::cookies::shouldSend(c, "example.com", "https://example.com/"));
}

TEST(Cookies, host_comparison_ignores_case_and_port) {
    const auto c = parsed("sid=abc123");
    EXPECT_TRUE(ce::cookies::shouldSend(c, "Example.com", "https://EXAMPLE.com:8443/"));
    EXPECT_EQ(ce::cookies::hostOf("https://User@API.Example.com:8443/p?q"), "api.example.com");
}

// ─── Jar cookies with their scope ───────────────────────────────────────────

namespace {

[[nodiscard]] std::vector<ce::JarCookie> absorbed(
    const std::vector<std::pair<std::string, std::string>>& headers, std::string_view url) {
    ce::RunContext ctx;
    const ce::ActorId actor{"user"};
    ce::cookies::absorbSetCookies(headers, actor, ctx, url);
    return ctx.jarCookies(actor);
}

}  // namespace

TEST(CookieJar, absorbed_cookie_records_the_setting_host_and_attributes) {
    const auto jar =
        absorbed({{"Set-Cookie", "sid=abc123; Domain=.Example.com; Path=/api; Secure"}},
                 "https://API.example.com:8443/login");
    ASSERT_EQ(jar.size(), 1U);
    EXPECT_EQ(jar.front().setterHost, "api.example.com");
    EXPECT_EQ(jar.front().domain, "example.com");
    EXPECT_EQ(jar.front().path, "/api");
    EXPECT_TRUE(jar.front().secure);
}

TEST(CookieJar, request_header_holds_only_the_cookies_that_belong_to_the_request) {
    const auto jar = absorbed({{"Set-Cookie", "host=h1"},
                               {"Set-Cookie", "dom=d1; Domain=example.com"},
                               {"Set-Cookie", "sec=s1; Secure"},
                               {"Set-Cookie", "deep=p1; Path=/api"}},
                              "https://a.example.com/login");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://a.example.com/api/x"),
              "deep=p1; dom=d1; host=h1; sec=s1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "http://a.example.com/"), "dom=d1; host=h1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://b.example.com/"), "dom=d1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://evil.test/"), "");
}

TEST(CookieJar, cookie_seeded_without_a_response_is_sent_to_any_host) {
    ce::RunContext ctx;
    const ce::ActorId actor{"user"};
    ctx.setCookie(actor, "seeded", "v1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(ctx.jarCookies(actor), "https://any.test/"),
              "seeded=v1");
}

TEST(CookieJar, expired_cookie_is_not_sent) {
    const auto jar = absorbed({{"Set-Cookie", "brief=b1; Max-Age=60"}}, "https://a.test/");
    ASSERT_EQ(jar.size(), 1U);
    ASSERT_TRUE(jar.front().expiresAt.has_value());
    const auto later = std::chrono::system_clock::now() + std::chrono::minutes{2};
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://a.test/"), "brief=b1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://a.test/", later), "");
}

TEST(CookieJar, removal_from_another_host_leaves_the_cookie_alone) {
    ce::RunContext ctx;
    const ce::ActorId actor{"user"};
    ce::cookies::absorbSetCookies({{"Set-Cookie", "sid=a1"}}, actor, ctx, "https://a.test/");
    ce::cookies::absorbSetCookies(
        {{"Set-Cookie", "sid=gone; Max-Age=0"}}, actor, ctx, "https://b.test/");
    EXPECT_EQ(ctx.cookies(actor).at("sid"), "a1");
    ce::cookies::absorbSetCookies(
        {{"Set-Cookie", "sid=gone; Max-Age=0"}}, actor, ctx, "https://a.test/");
    EXPECT_FALSE(ctx.cookies(actor).contains("sid"));
}

TEST(CookieJar, same_name_from_two_hosts_keeps_both) {
    ce::RunContext ctx;
    const ce::ActorId actor{"user"};
    ce::cookies::absorbSetCookies({{"Set-Cookie", "sid=a1"}}, actor, ctx, "https://a.test/");
    ce::cookies::absorbSetCookies({{"Set-Cookie", "sid=b1"}}, actor, ctx, "https://b.test/");
    const auto jar = ctx.jarCookies(actor);
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://a.test/"), "sid=a1");
    EXPECT_EQ(ce::cookies::requestHeaderFor(jar, "https://b.test/"), "sid=b1");
    EXPECT_EQ(ctx.cookies(actor).at("sid"), "b1") << "name view: the latest wins";
}

TEST(CookieJar, absorbed_cookie_value_is_registered_for_redaction) {
    ce::RunContext ctx;
    ce::cookies::absorbSetCookies(
        {{"Set-Cookie", "sid=cookie-secret-123"}}, ce::ActorId{"user"}, ctx, "https://a.test/");
    EXPECT_EQ(ctx.secretRegistry().redact("x cookie-secret-123 y").find("cookie-secret-123"),
              std::string::npos);
}
