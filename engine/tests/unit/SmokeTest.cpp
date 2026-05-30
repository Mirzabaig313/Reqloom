// Engine smoke test: validates that the public surface compiles, links,
// and produces stable error-code strings. Real integration tests
// are added in Phase 1.
//
// Each assertion fails without the production code under test:
//   - ErrorCodeStringsAreStable    → fails if ErrorCodes.cpp mapping breaks
//   - RetryabilityMatchesSpec      → fails if isRetryable() classification breaks
//   - RunContextSessionLifecycle   → fails if RunContext session API breaks
//   - RunContextExtractionCache    → fails if RunContext extraction API breaks
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

namespace ce = chainapi::engine;

TEST(EngineSmoke, ErrorCodeStringsAreStable) {
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::Cycle), "E_CYCLE");
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::SessionRefreshFailed), "E_SESSION_REFRESH_FAILED");
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::Http5xx), "E_HTTP_5XX");
}

TEST(EngineSmoke, FromCodeStringRoundTripsEveryCode) {
    // fromCodeString is the inverse used by the history store to
    // reconstruct StepFailed.code from persisted JSON. Every code must
    // round-trip; an unknown string yields nullopt.
    for (const auto code : {ce::ErrorCode::SchemaInvalid,
                            ce::ErrorCode::Cycle,
                            ce::ErrorCode::RefUndefined,
                            ce::ErrorCode::ExtractionFailed,
                            ce::ErrorCode::SessionRefreshFailed,
                            ce::ErrorCode::Cancelled,
                            ce::ErrorCode::LlmResponseInvalid}) {
        const auto round = ce::fromCodeString(ce::toCodeString(code));
        ASSERT_TRUE(round.has_value());
        EXPECT_EQ(*round, code);
    }
    EXPECT_FALSE(ce::fromCodeString("E_NOT_A_REAL_CODE").has_value());
    EXPECT_FALSE(ce::fromCodeString("").has_value());
}

TEST(EngineSmoke, RetryabilityMatchesSpec) {
    // 5xx and network timeouts retry; 4xx and schema errors do not.
    EXPECT_TRUE(ce::isRetryable(ce::ErrorCode::Http5xx));
    EXPECT_TRUE(ce::isRetryable(ce::ErrorCode::NetworkTimeout));
    EXPECT_FALSE(ce::isRetryable(ce::ErrorCode::Http4xx));
    EXPECT_FALSE(ce::isRetryable(ce::ErrorCode::Cycle));
    EXPECT_FALSE(ce::isRetryable(ce::ErrorCode::HookFailure));
}

TEST(EngineSmoke, RunContextSessionLifecycle) {
    ce::RunContext ctx;
    const ce::ActorId vendor{"vendor"};

    EXPECT_EQ(ctx.session(vendor), nullptr);

    ce::ActorSession session;
    session.state = ce::ActorSession::State::Live;
    session.variables["token"] = "test-token";
    ctx.putSession(vendor, session);

    ASSERT_NE(ctx.session(vendor), nullptr);
    EXPECT_EQ(ctx.session(vendor)->variables.at("token"), "test-token");

    ctx.invalidateSession(vendor);
    EXPECT_EQ(ctx.session(vendor), nullptr);
}

TEST(EngineSmoke, RunContextExtractionCacheIsIndexable) {
    ce::RunContext ctx;
    const ce::ResourceId order{"order"};

    EXPECT_TRUE(ctx.instances(order).empty());

    ce::ResourceInstance inst1;
    inst1.variables["order_id"] = "ord-1";
    ctx.appendInstance(order, inst1);

    ce::ResourceInstance inst2;
    inst2.variables["order_id"] = "ord-2";
    ctx.appendInstance(order, inst2);

    ASSERT_EQ(ctx.instances(order).size(), 2u);
    EXPECT_EQ(ctx.instances(order)[0].variables.at("order_id"), "ord-1");
    EXPECT_EQ(ctx.instances(order)[1].variables.at("order_id"), "ord-2");
}

TEST(EngineSmoke, ChainApiErrorIsCarriedThroughExpected) {
    // Construct a ChainApiError and round-trip it through std::expected
    // to confirm the error-channel idiom compiles end-to-end.
    auto failingOperation = []() -> std::expected<int, ce::ChainApiError> {
        return std::unexpected(ce::ChainApiError{ce::ErrorCode::VarUnresolved,
                                                 ce::ErrorClass::Resolution,
                                                 "missing: order.order_id; last set by: never"});
    };

    auto result = failingOperation();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Resolution);
    EXPECT_NE(result.error().detail.find("order.order_id"), std::string::npos);
}

// ─── Cookie jar API ─────────────────────────────────────────────────────────
//
// Per-actor jar lives on RunContext alongside the session cache. It
// accumulates Set-Cookie values across the run so subsequent operations
// AS the same actor automatically carry the cookie.

TEST(EngineSmoke, RunContextCookieJarIsPerActor) {
    ce::RunContext ctx;
    const ce::ActorId admin{"admin"};
    const ce::ActorId customer{"customer"};

    EXPECT_TRUE(ctx.cookies(admin).empty());

    ctx.setCookie(admin, "session", "admin-sess-1");
    ctx.setCookie(customer, "session", "cust-sess-1");

    // Each actor sees its own cookie, not the other actor's.
    ASSERT_EQ(ctx.cookies(admin).size(), 1u);
    EXPECT_EQ(ctx.cookies(admin).at("session"), "admin-sess-1");
    ASSERT_EQ(ctx.cookies(customer).size(), 1u);
    EXPECT_EQ(ctx.cookies(customer).at("session"), "cust-sess-1");
}

TEST(EngineSmoke, RunContextCookieJarLastWriteWinsOnNameCollision) {
    // Mirrors RFC 6265 §5.3 step 11. The executor relies on this when
    // the same response carries two Set-Cookie entries for one name.
    ce::RunContext ctx;
    const ce::ActorId actor{"u"};

    ctx.setCookie(actor, "tok", "old");
    ctx.setCookie(actor, "tok", "new");

    ASSERT_EQ(ctx.cookies(actor).size(), 1u);
    EXPECT_EQ(ctx.cookies(actor).at("tok"), "new");
}

TEST(EngineSmoke, RunContextInvalidateSessionAlsoDropsCookies) {
    // 401-recovery invalidates the session and re-auths. Carrying the
    // pre-recovery jar forward would leak stale cookies onto the
    // retry, so invalidateSession drops the jar too.
    ce::RunContext ctx;
    const ce::ActorId actor{"u"};

    ce::ActorSession sess;
    sess.state = ce::ActorSession::State::Live;
    ctx.putSession(actor, sess);
    ctx.setCookie(actor, "session", "stale");

    ASSERT_FALSE(ctx.cookies(actor).empty());
    ctx.invalidateSession(actor);
    EXPECT_TRUE(ctx.cookies(actor).empty());
    EXPECT_EQ(ctx.session(actor), nullptr);
}

TEST(EngineSmoke, RunContextCookieJarIgnoresEmptyName) {
    // Defensive: a malformed Set-Cookie that produces an empty name
    // should never enter the jar. The Cookies::parseSetCookie helper
    // already returns nullopt for these, but this is a belt-and-braces
    // check that the jar API itself is also safe to call directly.
    ce::RunContext ctx;
    const ce::ActorId actor{"u"};

    ctx.setCookie(actor, "", "value");
    EXPECT_TRUE(ctx.cookies(actor).empty());
}

TEST(EngineSmoke, RunContextClearCookiesDropsTheJarWithoutTouchingSession) {
    ce::RunContext ctx;
    const ce::ActorId actor{"u"};

    ce::ActorSession sess;
    sess.state = ce::ActorSession::State::Live;
    sess.variables["token"] = "alive";
    ctx.putSession(actor, sess);
    ctx.setCookie(actor, "tok", "v");

    ctx.clearCookies(actor);

    EXPECT_TRUE(ctx.cookies(actor).empty());
    ASSERT_NE(ctx.session(actor), nullptr) << "clearCookies must not touch the session";
    EXPECT_EQ(ctx.session(actor)->variables.at("token"), "alive");
}
