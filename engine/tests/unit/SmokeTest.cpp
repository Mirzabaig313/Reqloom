// Engine smoke test: validates that the public surface compiles, links,
// and produces stable error-code strings. Real integration tests
// (Engine Req §8) are added in Phase 1.
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
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::SessionRefreshFailed),
              "E_SESSION_REFRESH_FAILED");
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::Http5xx), "E_HTTP_5XX");
}

TEST(EngineSmoke, RetryabilityMatchesSpec) {
    // Engine Req §3.5: 5xx, network timeouts retry; 4xx and schema errors do not.
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
        return std::unexpected(ce::ChainApiError{
            ce::ErrorCode::VarUnresolved,
            ce::ErrorClass::Resolution,
            "missing: order.order_id; last set by: never"});
    };

    auto result = failingOperation();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Resolution);
    EXPECT_NE(result.error().detail.find("order.order_id"), std::string::npos);
}
