// QuickJsHookRunner unit tests — exercises the JS sandbox in isolation
// from the executor. Confirms:
//   - Inline-style hooks can mutate ctx.request.headers / .body / .url
//   - Module-style (`export default`) hooks work the same
//   - post_response hooks see ctx.response and can rewrite body/status
//   - Helper bindings expose codecs (base64/hex/url), HMAC, and JWT
//   - Sandbox: 1-second budget enforced, no `require`, no `process`,
//     no filesystem (we just assert the global doesn't exist)
//   - Failure paths surface E_HOOK_FAILURE / E_HOOK_TIMEOUT cleanly
//
// Each test fails on the parent commit — `QuickJsHookRunner` was a stub
// that returned the input unchanged.

#include "infrastructure/hooks/QuickJsHookRunner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace ce = reqloom::engine;

namespace {

[[nodiscard]] ce::HookContext baseContext() {
    ce::HookContext c;
    c.request.method = ce::HttpMethod::Post;
    c.request.url = "https://api.example.com/v1/orders";
    c.request.headers["X-Existing"] = "from-template";
    c.request.body = R"({"qty":1})";
    c.env["baseUrl"] = "https://api.example.com";
    c.variables["user"] = {{"token", "abc123"}};
    return c;
}

}  // namespace

// ─── Inline-style mutation ───────────────────────────────────────────────────

TEST(QuickJsHookRunner, inline_pre_request_can_set_a_header) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "ctx.request.headers['X-Signature'] = 'sig-' + ctx.request.body.length;";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error().detail);

    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Signature"), "sig-9");
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Existing"), "from-template");
}

TEST(QuickJsHookRunner, inline_pre_request_can_replace_body_and_url) {
    ce::QuickJsHookRunner runner;
    const std::string script = R"(
        ctx.request.body = JSON.stringify({wrapped: JSON.parse(ctx.request.body)});
        ctx.request.url = ctx.env.baseUrl + '/v2/orders';
    )";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    ASSERT_TRUE(outcome->mutatedRequest.body.has_value());
    EXPECT_EQ(*outcome->mutatedRequest.body, R"({"wrapped":{"qty":1}})");
    EXPECT_EQ(outcome->mutatedRequest.url, "https://api.example.com/v2/orders");
}

TEST(QuickJsHookRunner, inline_pre_request_reads_actor_variables) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "ctx.request.headers['Authorization'] = 'Bearer ' + ctx.actors.user.token;";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->mutatedRequest.headers.at("Authorization"), "Bearer abc123");
}

TEST(QuickJsHookRunner, inline_pre_request_reads_secrets) {
    ce::QuickJsHookRunner runner;
    auto hctx = baseContext();
    hctx.secrets["API_KEY"] = "sk_live_zzz";

    const std::string script = "ctx.request.headers['X-Api-Key'] = 'Bearer ' + ctx.secret.API_KEY;";

    auto outcome = runner.runPreRequest(script, std::move(hctx));
    ASSERT_TRUE(outcome.has_value()) << (outcome ? "" : outcome.error().detail);
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Api-Key"), "Bearer sk_live_zzz");
}

// ─── Module-style mutation (export default) ─────────────────────────────────

TEST(QuickJsHookRunner, module_default_export_runs) {
    ce::QuickJsHookRunner runner;
    const std::string script = R"(
export default function (ctx) {
    ctx.request.headers['X-From-Module'] = 'yes';
}
)";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-From-Module"), "yes");
}

// ─── post_response ───────────────────────────────────────────────────────────

TEST(QuickJsHookRunner, post_response_can_unwrap_body_for_extraction) {
    ce::QuickJsHookRunner runner;

    auto hctx = baseContext();
    ce::HookResponseView resp;
    resp.status = 200;
    resp.headers["Content-Type"] = "application/json";
    resp.body = R"({"envelope":{"data":{"id":42}}})";
    hctx.response = resp;

    const std::string script = R"(
        const inner = JSON.parse(ctx.response.body).envelope;
        ctx.response.body = JSON.stringify(inner);
    )";

    auto outcome = runner.runPostResponse(script, hctx);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    ASSERT_TRUE(outcome->mutatedResponse.has_value());
    EXPECT_EQ(outcome->mutatedResponse->body, R"({"data":{"id":42}})");
    EXPECT_EQ(outcome->mutatedResponse->status, 200);
}

// ─── Helper bindings ─────────────────────────────────────────────────────────

TEST(QuickJsHookRunner, base64_helper_round_trips) {
    ce::QuickJsHookRunner runner;
    const std::string script = R"(
        const enc = ctx.base64.encode('hello world');
        const dec = ctx.base64.decode(enc);
        ctx.request.headers['X-Roundtrip'] = enc + '|' + dec;
    )";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Roundtrip"), "aGVsbG8gd29ybGQ=|hello world");
}

TEST(QuickJsHookRunner, hmac_helper_returns_stable_hex) {
    ce::QuickJsHookRunner runner;
    // RFC 4231 §4.2 HMAC-SHA-256 test case 1:
    //   key  = 20 bytes of 0x0b   data = "Hi There"
    //   mac  = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
    const std::string script = R"(
        const key = '\x0b'.repeat(20);
        ctx.request.headers['X-Mac'] = ctx.hmac.sha256(key, 'Hi There');
    )";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Mac"),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(QuickJsHookRunner, jwt_sign_produces_three_segment_token) {
    ce::QuickJsHookRunner runner;
    const std::string script = R"(
        ctx.request.headers['X-Token'] =
            ctx.jwt.sign({sub: 'u1', exp: 9999999999}, 'k1', 'HS256');
    )";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& tok = outcome->mutatedRequest.headers.at("X-Token");

    int dots = 0;
    for (char ch : tok) {
        if (ch == '.') ++dots;
    }
    EXPECT_EQ(dots, 2) << "JWT should have header.payload.signature shape, got: " << tok;
    // Signature segment must not be empty.
    EXPECT_NE(tok.back(), '.');
    EXPECT_GT(tok.size(), 30u);
}

TEST(QuickJsHookRunner, url_encode_helper_handles_special_chars) {
    ce::QuickJsHookRunner runner;
    const std::string script = "ctx.request.headers['X-Q'] = ctx.url.encode('a b/c?d=e');";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Q"), "a%20b%2Fc%3Fd%3De");
}

// ─── Sandbox boundaries ──────────────────────────────────────────────────────

TEST(QuickJsHookRunner, no_filesystem_or_process_globals_exposed) {
    ce::QuickJsHookRunner runner;
    const std::string script = R"(
        const checks = [];
        checks.push(typeof require);
        checks.push(typeof process);
        checks.push(typeof globalThis.os);
        checks.push(typeof globalThis.std);
        checks.push(typeof XMLHttpRequest);
        ctx.request.headers['X-Checks'] = checks.join(',');
    )";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->mutatedRequest.headers.at("X-Checks"),
              "undefined,undefined,undefined,undefined,undefined");
}

TEST(QuickJsHookRunner, runtime_error_surfaces_as_hook_failure) {
    ce::QuickJsHookRunner runner;
    const std::string script = "throw new Error('boom');";

    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(outcome.error().detail.find("boom"), std::string::npos);
}

TEST(QuickJsHookRunner, infinite_loop_trips_the_one_second_budget) {
    ce::QuickJsHookRunner runner;
    // Tight loop that never yields; the interrupt handler must fire.
    const std::string script = "for (;;) {}";

    const auto t0 = std::chrono::steady_clock::now();
    auto outcome = runner.runPreRequest(script, baseContext());
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookTimeout);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5)
        << "interrupt handler did not fire promptly";
}

TEST(QuickJsHookRunner, hooks_are_isolated_between_invocations) {
    ce::QuickJsHookRunner runner;

    const std::string seed = "globalThis.leak = 'pwned';";
    auto firstRun = runner.runPreRequest(seed, baseContext());
    ASSERT_TRUE(firstRun.has_value());

    const std::string check = "ctx.request.headers['X-Leak'] = String(globalThis.leak);";
    auto secondRun = runner.runPreRequest(check, baseContext());
    ASSERT_TRUE(secondRun.has_value()) << secondRun.error().detail;
    // A fresh runtime per invocation means `globalThis.leak` was never
    // set in the second runtime.
    EXPECT_EQ(secondRun->mutatedRequest.headers.at("X-Leak"), "undefined");
}

// ─── Timeout detection (D-12) ────────────────────────────────────────────────
// The timeout verdict must come from the interrupt handler actually firing,
// not from the text of whatever the hook threw.

TEST(QuickJsHookRunner, error_whose_message_says_interrupted_is_not_a_timeout) {
    ce::QuickJsHookRunner runner;
    auto inlineOutcome = runner.runPreRequest("throw new Error('interrupted');", baseContext());
    ASSERT_FALSE(inlineOutcome.has_value());
    EXPECT_EQ(inlineOutcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(inlineOutcome.error().detail.find("interrupted"), std::string::npos);

    auto moduleOutcome = runner.runPreRequest(
        "export default function (ctx) { throw new Error('interrupted'); }", baseContext());
    ASSERT_FALSE(moduleOutcome.has_value());
    EXPECT_EQ(moduleOutcome.error().code, ce::ErrorCode::HookFailure);
}

TEST(QuickJsHookRunner, module_infinite_loop_is_reported_as_timeout) {
    ce::QuickJsHookRunner runner;
    auto outcome =
        runner.runPreRequest("export default function (ctx) { for (;;) {} }", baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookTimeout);
}

// ─── Memory limit (D-17) ─────────────────────────────────────────────────────

TEST(QuickJsHookRunner, runaway_allocation_fails_as_hook_error) {
    ce::QuickJsHookRunner runner;
    // ~512 MiB if unbounded; the runtime memory limit must stop it early.
    const std::string script = R"(
        const keep = [];
        for (let i = 0; i < 32; i++) { keep.push(new Array(1 << 20).fill(i)); }
        ctx.request.headers['X-Done'] = String(keep.length);
    )";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value()) << "allocation loop was not stopped by a memory limit";
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(outcome.error().detail.find("out of memory"), std::string::npos)
        << outcome.error().detail;

    // The runner stays usable afterwards.
    auto next = runner.runPreRequest("ctx.request.headers['X-Ok'] = '1';", baseContext());
    ASSERT_TRUE(next.has_value()) << next.error().detail;
}

// ─── Async hooks (D-11) ──────────────────────────────────────────────────────

TEST(QuickJsHookRunner, async_module_hook_mutation_after_await_is_applied) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "export default async function (ctx) { await 0; ctx.request.headers.sig = 'x'; }";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    ASSERT_TRUE(outcome->mutatedRequest.headers.contains("sig"));
    EXPECT_EQ(outcome->mutatedRequest.headers.at("sig"), "x");
}

TEST(QuickJsHookRunner, inline_hook_returning_a_promise_is_awaited) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "return (async () => { await 0; ctx.request.headers.sig = 'y'; })();";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    ASSERT_TRUE(outcome->mutatedRequest.headers.contains("sig"));
    EXPECT_EQ(outcome->mutatedRequest.headers.at("sig"), "y");
}

TEST(QuickJsHookRunner, async_module_hook_rejection_fails_the_hook) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "export default async function (ctx) { await 0; throw new Error('async-boom'); }";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(outcome.error().detail.find("async-boom"), std::string::npos)
        << outcome.error().detail;
}

TEST(QuickJsHookRunner, async_hook_that_never_settles_fails_clearly) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "export default async function (ctx) { await new Promise(() => {}); }";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(outcome.error().detail.find("never settled"), std::string::npos)
        << outcome.error().detail;
}

TEST(QuickJsHookRunner, async_hook_looping_after_await_times_out) {
    ce::QuickJsHookRunner runner;
    const std::string script = "export default async function (ctx) { await 0; for (;;) {} }";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookTimeout);
}

TEST(QuickJsHookRunner, module_top_level_throw_reports_the_real_error) {
    ce::QuickJsHookRunner runner;
    const std::string script =
        "export const x = 1;\n"
        "throw new Error('top-level-boom');\n"
        "export default function (ctx) {}";
    auto outcome = runner.runPreRequest(script, baseContext());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::HookFailure);
    EXPECT_NE(outcome.error().detail.find("top-level-boom"), std::string::npos)
        << outcome.error().detail;
}
