// Hook typings emitter tests.
#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        const auto unique =
            "chainapi-typings-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
    inline static int counter_{0};
};

std::string readFile(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

ce::Project makeMinimalProject() {
    ce::Project p;
    p.name = "TypingsTarget";
    p.defaultEnvironment = "local";
    return p;
}

}  // namespace

TEST(HookTypingsEmitter, writes_chainapi_dts_to_target_directory) {
    ScratchDir scratch;
    const auto written = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    EXPECT_EQ(written->filename(), "chainapi.d.ts");
    EXPECT_TRUE(fs::exists(*written));
}

TEST(HookTypingsEmitter, refuses_to_overwrite_existing_file) {
    ScratchDir scratch;

    auto first = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(first.has_value());

    auto second = ce::emitHookTypings(scratch.path(),
                                      makeMinimalProject(),
                                      /*overwrite=*/false);
    ASSERT_FALSE(second.has_value());
    EXPECT_NE(second.error().detail.find("exists"), std::string::npos);

    auto third = ce::emitHookTypings(scratch.path(),
                                     makeMinimalProject(),
                                     /*overwrite=*/true);
    EXPECT_TRUE(third.has_value());
}

TEST(HookTypingsEmitter, declares_required_top_level_types) {
    ScratchDir scratch;
    const auto written = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(written.has_value());

    const auto body = readFile(*written);

    // Top-level surfaces hooks rely on.
    EXPECT_NE(body.find("namespace ChainApi"), std::string::npos);
    EXPECT_NE(body.find("interface Context"), std::string::npos);
    EXPECT_NE(body.find("interface MutableRequest"), std::string::npos);
    EXPECT_NE(body.find("interface ResponseView"), std::string::npos);
    EXPECT_NE(body.find("type PreRequestHook"), std::string::npos);
    EXPECT_NE(body.find("type PostResponseHook"), std::string::npos);
}

TEST(HookTypingsEmitter, surfaces_codec_and_crypto_helpers) {
    // The codecs (base64, hex, url) and crypto helpers (hmac, hash, jwt,
    // json) must be reachable on `ctx`.
    ScratchDir scratch;
    const auto written = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(written.has_value());
    const auto body = readFile(*written);

    for (const auto* sym : {"base64", "hex", "url", "hmac", "hash", "jwt", "json"}) {
        EXPECT_NE(body.find(sym), std::string::npos)
            << "expected `ctx." << sym << "` to appear in the typings";
    }
}

TEST(HookTypingsEmitter, marks_response_as_optional_for_pre_request_hooks) {
    // Pre-request hooks see no response; post-response hooks do. The
    // typings reflect that with `response?:` so editors don't suggest
    // accessing a nonexistent field in pre-request hooks.
    ScratchDir scratch;
    const auto written = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(written.has_value());
    const auto body = readFile(*written);

    EXPECT_NE(body.find("response?:"), std::string::npos);
}

TEST(HookTypingsEmitter, declares_jwt_alg_union_HS256_HS512) {
    // Only HS256 / HS512 are implemented — the typings must NOT promise
    // RS256 or ES256 before the binding is real.
    ScratchDir scratch;
    const auto written = ce::emitHookTypings(scratch.path(), makeMinimalProject());
    ASSERT_TRUE(written.has_value());
    const auto body = readFile(*written);

    EXPECT_NE(body.find("\"HS256\""), std::string::npos);
    EXPECT_NE(body.find("\"HS512\""), std::string::npos);
    EXPECT_EQ(body.find("\"RS256\""), std::string::npos)
        << "RS256 is not yet implemented; typings must not advertise it";
    EXPECT_EQ(body.find("\"ES256\""), std::string::npos);
}
