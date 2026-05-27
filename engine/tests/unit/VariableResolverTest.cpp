// Unit tests for the VariableResolver grammar upgrade.
#include "domain/VariableResolver.h"

#include <chainapi/engine/RunContext.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iomanip>
#include <regex>
#include <sstream>

namespace ce = chainapi::engine;

namespace {

ce::ResolveContext makeResolveCtx() {
    ce::ResolveContext rctx;
    rctx.envVars = {{"baseUrl", "https://api.test"}};
    rctx.secrets = {{"API_KEY", "shh"}};
    return rctx;
}

}  // namespace

TEST(VariableResolver, resolves_now_plus_offset_to_iso_in_the_future) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto before = std::chrono::system_clock::now();
    const auto result = resolver.resolve("at={{$.now+5m}}", ctx, rctx);
    const auto after = std::chrono::system_clock::now();

    ASSERT_TRUE(result.unresolved.empty()) << "first unresolved: " << result.unresolved.front();
    ASSERT_TRUE(result.output.starts_with("at="));

    // Must look like ISO 8601: YYYY-MM-DDTHH:MM:SSZ
    const std::regex iso(R"(^at=(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)$)");
    std::smatch m;
    ASSERT_TRUE(std::regex_match(result.output, m, iso))
        << "output not ISO 8601: " << result.output;

    // The resolved instant must be at least 5m after the call started
    // and no more than 5m + a generous drift after the call returned.
    std::tm tm{};
    std::istringstream is(m[1].str());
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    ASSERT_FALSE(is.fail()) << "could not parse: " << m[1].str();

    const auto resolvedTime = std::chrono::system_clock::from_time_t(timegm(&tm));
    EXPECT_GE(resolvedTime - before, std::chrono::seconds{5 * 60 - 1});
    EXPECT_LE(resolvedTime - after, std::chrono::seconds{5 * 60 + 5});
}

TEST(VariableResolver, resolves_now_minus_offset_to_iso_in_the_past) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto before = std::chrono::system_clock::now();
    const auto result = resolver.resolve("{{$.now-1h}}", ctx, rctx);

    ASSERT_TRUE(result.unresolved.empty());
    std::tm tm{};
    std::istringstream is(result.output);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    ASSERT_FALSE(is.fail()) << "could not parse: " << result.output;

    const auto resolvedTime = std::chrono::system_clock::from_time_t(timegm(&tm));
    EXPECT_LE(resolvedTime, before - std::chrono::seconds{3600 - 5});
}

TEST(VariableResolver, now_offset_tolerates_whitespace_around_operator) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto a = resolver.resolve("{{ $.now + 5m }}", ctx, rctx);
    const auto b = resolver.resolve("{{$.now+5m}}", ctx, rctx);

    EXPECT_TRUE(a.unresolved.empty());
    EXPECT_TRUE(b.unresolved.empty());
    // Both must parse to a valid ISO timestamp; we don't compare values
    // because the wall clock advances between calls.
    EXPECT_FALSE(a.output.empty());
    EXPECT_FALSE(b.output.empty());
}

TEST(VariableResolver, rejects_malformed_duration_unit) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto result = resolver.resolve("{{$.now+5x}}", ctx, rctx);

    // The offset is bad → builtin matcher must NOT silently produce a
    // value. The reference is left in place and reported unresolved.
    ASSERT_EQ(result.unresolved.size(), 1u);
    EXPECT_EQ(result.unresolved.front(), "$.now+5x");
    EXPECT_EQ(result.output, "{{$.now+5x}}");
}

TEST(VariableResolver, rejects_negative_duration_value) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    // "{{$.now+-5m}}" — the second '-' is part of the duration, which
    // must be rejected. The first '+' is the offset operator; what
    // follows is "-5m" which is not a valid duration literal.
    const auto result = resolver.resolve("{{$.now+-5m}}", ctx, rctx);
    ASSERT_EQ(result.unresolved.size(), 1u);
}

TEST(VariableResolver, preserves_uuid_and_now_behavior) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto uuid = resolver.resolve("{{$.uuid}}", ctx, rctx);
    EXPECT_TRUE(uuid.unresolved.empty());
    // UUID v4 shape: 8-4-4-4-12 hex chars
    const std::regex uuidRe(
        R"([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})");
    EXPECT_TRUE(std::regex_match(uuid.output, uuidRe)) << uuid.output;

    const auto now = resolver.resolve("{{$.now}}", ctx, rctx);
    EXPECT_TRUE(now.unresolved.empty());
    const std::regex isoRe(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
    EXPECT_TRUE(std::regex_match(now.output, isoRe)) << now.output;
}

TEST(VariableResolver, preserves_indexed_resource_lookup) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    ctx.appendInstance(ce::ResourceId{"order"}, ce::ResourceInstance{{{"id", "ord-1"}}});
    ctx.appendInstance(ce::ResourceId{"order"}, ce::ResourceInstance{{{"id", "ord-2"}}});

    EXPECT_EQ(resolver.resolve("{{order[1].id}}", ctx, rctx).output, "ord-1");
    EXPECT_EQ(resolver.resolve("{{order[2].id}}", ctx, rctx).output, "ord-2");
    // Most-recent fallback when no index is specified.
    EXPECT_EQ(resolver.resolve("{{order.id}}", ctx, rctx).output, "ord-2");
    // Out of range stays unresolved, not throws.
    const auto miss = resolver.resolve("{{order[99].id}}", ctx, rctx);
    EXPECT_FALSE(miss.unresolved.empty());
}

TEST(VariableResolver, preserves_secret_and_env_resolution) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    EXPECT_EQ(resolver.resolve("{{env.baseUrl}}", ctx, rctx).output, "https://api.test");
    EXPECT_EQ(resolver.resolve("{{secret.API_KEY}}", ctx, rctx).output, "shh");
}

// ─── Slice 2c: base64 / hex / url codecs ─────────────────────────────────────
//
// Each test fails on the pre-2c commit:
//   - $.base64.encode("...") is parsed as a single literal that doesn't
//     match $.now / $.uuid / $.env / $.faker → unresolved + left in place.

TEST(VariableResolver, base64_encodes_quoted_literal_with_padding) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    // Classic basic-auth example from RFC 7617.
    const auto r = resolver.resolve(R"({{$.base64.encode("Aladdin:open sesame")}})", ctx, rctx);

    ASSERT_TRUE(r.unresolved.empty()) << "first: " << r.unresolved.front();
    EXPECT_EQ(r.output, "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
}

TEST(VariableResolver, base64_encodes_secret_reference) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    auto rctx = makeResolveCtx();
    rctx.secrets["BASIC_PAIR"] = "user:pw";

    const auto r = resolver.resolve("{{$.base64.encode(secret.BASIC_PAIR)}}", ctx, rctx);

    ASSERT_TRUE(r.unresolved.empty());
    EXPECT_EQ(r.output, "dXNlcjpwdw==");
}

TEST(VariableResolver, base64_encode_decode_round_trip) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    auto rctx = makeResolveCtx();
    rctx.envVars["RAW"] = "Hello, ChainAPI!";
    rctx.envVars["B64"] = "SGVsbG8sIENoYWluQVBJIQ==";

    const auto enc = resolver.resolve("{{$.base64.encode(env.RAW)}}", ctx, rctx);
    EXPECT_EQ(enc.output, "SGVsbG8sIENoYWluQVBJIQ==");

    const auto dec = resolver.resolve("{{$.base64.decode(env.B64)}}", ctx, rctx);
    EXPECT_EQ(dec.output, "Hello, ChainAPI!");
}

TEST(VariableResolver, base64_decode_rejects_invalid_input) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    // '@' is not in the base64 alphabet; decode returns nullopt → unresolved.
    const auto r = resolver.resolve(R"({{$.base64.decode("@@@@")}})", ctx, rctx);
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_EQ(r.unresolved.front(), R"($.base64.decode("@@@@"))");
}

TEST(VariableResolver, hex_encodes_lowercase) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto r = resolver.resolve(R"({{$.hex.encode("AB")}})", ctx, rctx);
    EXPECT_EQ(r.output, "4142");
}

TEST(VariableResolver, hex_decodes_mixed_case_round_trip) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    auto rctx = makeResolveCtx();
    rctx.envVars["BINARY"] = "deadBEEF";

    const auto r = resolver.resolve("{{$.hex.decode(env.BINARY)}}", ctx, rctx);
    ASSERT_TRUE(r.unresolved.empty());
    EXPECT_EQ(r.output, std::string("\xDE\xAD\xBE\xEF", 4));
}

TEST(VariableResolver, hex_decode_rejects_odd_length) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto r = resolver.resolve(R"({{$.hex.decode("abc")}})", ctx, rctx);
    EXPECT_EQ(r.unresolved.size(), 1u);
}

TEST(VariableResolver, url_encodes_reserved_chars) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto r = resolver.resolve(R"({{$.url.encode("hello world / ?&=")}})", ctx, rctx);
    // Per RFC 3986: space → %20, '/' → %2F, '?' → %3F, '&' → %26, '=' → %3D.
    EXPECT_EQ(r.output, "hello%20world%20%2F%20%3F%26%3D");
}

TEST(VariableResolver, url_decodes_plus_and_percent) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto a = resolver.resolve(R"({{$.url.decode("a+b%20c")}})", ctx, rctx);
    EXPECT_EQ(a.output, "a b c");

    const auto bad = resolver.resolve(R"({{$.url.decode("%ZZ")}})", ctx, rctx);
    EXPECT_EQ(bad.unresolved.size(), 1u);
}

TEST(VariableResolver, codec_unknown_function_is_unresolved) {
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    // $.base64.flip is not a defined op — must surface as unresolved
    // rather than returning a silently-empty string.
    const auto r = resolver.resolve(R"({{$.base64.flip("x")}})", ctx, rctx);
    EXPECT_EQ(r.unresolved.size(), 1u);
}

// ─── Review-fix regression tests ────────────────────────────────────────────

TEST(VariableResolver, rejects_overflowing_duration_without_ub) {
    // M5 regression. parseDuration used `value * 86400` on a long long;
    // an absurdly large numeric component would invoke signed overflow
    // UB (caught by UBSan). Now returns nullopt cleanly.
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    const auto rctx = makeResolveCtx();

    const auto huge = resolver.resolve("{{$.now+999999999999999999d}}", ctx, rctx);
    // Must surface as unresolved rather than crash or return a garbage
    // ISO timestamp.
    ASSERT_EQ(huge.unresolved.size(), 1u);
    EXPECT_EQ(huge.unresolved.front(), "$.now+999999999999999999d");
}
