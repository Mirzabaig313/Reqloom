// SecretEnvTagTest — the `!secret NAME` YAML tag in environment files.
//
// `key: !secret NAME` must bind the env var to a keychain secret, parsed
// into the reference `{{secret.NAME}}` (PRD §5.4). Before the fix, yaml-cpp
// dropped the tag and the var held the literal string "NAME". These cover
// the parser conversion, the collectSecretReferences pickup, and the
// resolver's env→secret re-expansion.
//
// Each test fails on the parent commit, where `!secret` was a no-op.
#include "domain/DependencyResolver.h"
#include "domain/VariableResolver.h"

#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class ScratchProject {
public:
    explicit ScratchProject(const std::string& body) {
        path_ = chainapi::tests::uniqueTempPath("chainapi-secret-env");
        fs::create_directories(path_);
        std::ofstream{path_ / "chainapi.yaml"} << body;
    }
    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;

    [[nodiscard]] fs::path yaml() const { return path_ / "chainapi.yaml"; }

private:
    fs::path path_;
};

constexpr const char* kSecretEnvYaml = R"YAML(
version: 1
name: SecretEnvTag
default_environment: local

environment:
  baseUrl: http://placeholder
  api_key: !secret SERVICE_API_KEY
  plain_value: literal-text

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        expect_status: 200
)YAML";

}  // namespace

TEST(SecretEnvTag, secret_tag_parses_into_secret_reference) {
    ScratchProject project(kSecretEnvYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    const auto& vars = loaded->environments.at("local");
    // The tagged value becomes a {{secret.X}} reference, not the literal "SERVICE_API_KEY".
    EXPECT_EQ(vars.at("api_key"), "{{secret.SERVICE_API_KEY}}");
    // Untagged scalars are unchanged.
    EXPECT_EQ(vars.at("plain_value"), "literal-text");
    EXPECT_EQ(vars.at("baseUrl"), "http://placeholder");
}

TEST(SecretEnvTag, secret_env_reference_is_collected_for_preloading) {
    ScratchProject project(kSecretEnvYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    const auto names = ce::DependencyResolver::collectSecretReferences(*loaded);
    // The secret behind the env var must be discoverable so run() pre-loads it.
    EXPECT_NE(std::find(names.begin(), names.end(), "SERVICE_API_KEY"), names.end());
}

TEST(SecretEnvTag, env_reference_to_secret_var_resolves_through_to_value) {
    // {{env.api_key}} → {{secret.SERVICE_API_KEY}} → the secret value.
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    ce::ResolveContext rctx;
    rctx.envVars = {{"api_key", "{{secret.SERVICE_API_KEY}}"}};
    rctx.secrets = {{"SERVICE_API_KEY", "sk_live_abc"}};

    const auto r = resolver.resolve("Bearer {{env.api_key}}", ctx, rctx);
    EXPECT_EQ(r.output, "Bearer sk_live_abc");
    EXPECT_TRUE(r.unresolved.empty());
}

TEST(SecretEnvTag, env_value_referencing_missing_secret_stays_unresolved) {
    // The secret isn't loaded → the embedded reference can't resolve, so
    // the whole `{{env.api_key}}` ref is reported unresolved and preserved
    // as a placeholder rather than emitting a half-expanded string.
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    ce::ResolveContext rctx;
    rctx.envVars = {{"api_key", "{{secret.MISSING}}"}};

    const auto r = resolver.resolve("{{env.api_key}}", ctx, rctx);
    EXPECT_EQ(r.output, "{{env.api_key}}");
    ASSERT_FALSE(r.unresolved.empty());
    EXPECT_EQ(r.unresolved.front(), "env.api_key");
}

TEST(SecretEnvTag, self_referential_env_value_does_not_infinite_loop) {
    // A pathological env value that references itself must terminate via
    // the depth guard rather than recursing forever.
    ce::VariableResolver resolver;
    ce::RunContext ctx;
    ce::ResolveContext rctx;
    rctx.envVars = {{"loop", "{{env.loop}}"}};

    const auto r = resolver.resolve("{{env.loop}}", ctx, rctx);
    // We don't assert an exact string — only that resolution returns.
    SUCCEED();
}
