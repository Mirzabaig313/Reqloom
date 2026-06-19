// Public hook dry-run API tests — exercise reqloom/engine/Hook.h, the
// stateless entry point an editor uses to validate a hook and preview its
// mutations. The sandbox itself is covered elsewhere; here we assert the
// public wrapper's contract: mutated request/response on success, and
// HookFailure for bad input / script errors.

#include <reqloom/engine/Hook.h>

#include <gtest/gtest.h>

namespace ce = reqloom::engine;

namespace {

ce::HookDryRunInput preRequest(std::string script) {
    ce::HookDryRunInput in;
    in.phase = ce::HookPhase::PreRequest;
    in.script = std::move(script);
    in.request.method = ce::HttpMethod::Post;
    in.request.url = "https://api.test/orders";
    in.request.headers = {{"Content-Type", "application/json"}};
    in.request.body = std::string{R"({"q":1})"};
    return in;
}

TEST(DryRunHook, pre_request_header_mutation_is_returned) {
    auto in = preRequest("ctx.request.headers['X-Sig'] = 'abc123';");
    in.secrets = {{"SIGNING_KEY", "shh"}};

    const auto out = ce::dryRunHook(in);
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    ASSERT_TRUE(out->request.headers.contains("X-Sig"));
    EXPECT_EQ(out->request.headers.at("X-Sig"), "abc123");
}

TEST(DryRunHook, pre_request_can_read_secret_into_header) {
    auto in = preRequest("ctx.request.headers['Authorization'] = 'Bearer ' + ctx.secret['TOKEN'];");
    in.secrets = {{"TOKEN", "sk-live-xyz"}};

    const auto out = ce::dryRunHook(in);
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->request.headers.at("Authorization"), "Bearer sk-live-xyz");
}

TEST(DryRunHook, post_response_body_mutation_is_returned) {
    ce::HookDryRunInput in;
    in.phase = ce::HookPhase::PostResponse;
    in.script = "ctx.response.body = 'rewritten';";
    in.request.url = "https://api.test/orders";
    in.response = ce::HookSampleResponse{200, {{"Content-Type", "text/plain"}}, "original"};

    const auto out = ce::dryRunHook(in);
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    ASSERT_TRUE(out->response.has_value());
    EXPECT_EQ(out->response->body, "rewritten");
}

TEST(DryRunHook, empty_script_is_hook_failure) {
    const auto out = ce::dryRunHook(preRequest(""));
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ce::ErrorCode::HookFailure);
}

TEST(DryRunHook, post_response_without_response_is_hook_failure) {
    ce::HookDryRunInput in;
    in.phase = ce::HookPhase::PostResponse;
    in.script = "ctx.response.body = 'x';";
    const auto out = ce::dryRunHook(in);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ce::ErrorCode::HookFailure);
}

TEST(DryRunHook, script_syntax_error_is_hook_failure) {
    const auto out = ce::dryRunHook(preRequest("this is not valid javascript {{{"));
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ce::ErrorCode::HookFailure);
}

}  // namespace
