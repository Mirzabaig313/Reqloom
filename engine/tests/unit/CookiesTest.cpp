// Cookies — unit tests for the small parsing helpers in
// engine/src/application/Cookies.h.
//
// Each test pins one corner of the contract — Set-Cookie parsing,
// collision rules, and request-header formatting. The integration
// tests in CookieJarTest.cpp prove the executor wires this all the
// way through to the wire.

#include "application/Cookies.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ce = chainapi::engine;

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
