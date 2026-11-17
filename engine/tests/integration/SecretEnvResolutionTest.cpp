// SecretEnvResolutionTest — `{{secret.NAME}}` resolves from `REQLOOM_SECRET_<NAME>` first, then
// the OS keychain, then fails as VarUnresolved naming the secret (PRD FR-11.5). With no keychain
// the failure says so and names the variable to set (FR-11.6). A bare `<NAME>` variable is never
// read. All values are fake.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>
#include <reqloom/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;

namespace {

/// Set (or with nullopt, remove) an environment variable for the life of this object.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::optional<std::string>& value) : name_(std::move(name)) {
        set(value);
    }
    ~ScopedEnv() { set(std::nullopt); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    void set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            setenv(name_.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }
    std::string name_;
};

class CapturingHttpClient final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& req) override {
        requests.push_back(req);
        ce::HttpResponse resp;
        resp.status = 200;
        resp.body = R"({"ok":true,"token":"tok"})";
        return resp;
    }
    std::vector<ce::HttpRequest> requests;
};

class FakeSecretStore final : public ce::SecretStore {
public:
    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        ++reads;
        lastUnreachable = unreachableReads > 0;
        if (unreachableReads > 0) {
            --unreachableReads;
            return std::optional<std::string>{};
        }
        if (fail) {
            return std::unexpected(ce::ReqloomError{
                ce::ErrorCode::SecretAccessFailed, ce::ErrorClass::Auth, "fake backend failure"});
        }
        if (auto it = values.find(name); it != values.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                const std::string& value) override {
        values[name] = value;
        return {};
    }
    std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        values.erase(name);
        return {};
    }
    [[nodiscard]] bool available() const noexcept override { return backendPresent; }
    [[nodiscard]] bool lastReadUnreachable() const noexcept override { return lastUnreachable; }

    std::map<std::string, std::string> values;
    int reads{0};
    bool backendPresent{true};
    int unreachableReads{0};
    bool lastUnreachable{false};
    bool fail{false};
};

class CountingTrustStore final : public ce::ProjectTrustStore {
public:
    std::expected<std::optional<std::filesystem::path>, ce::ReqloomError> folderFor(
        const std::string& /*projectId*/) override {
        ++calls;
        return std::optional<std::filesystem::path>{};
    }
    std::expected<std::filesystem::path, ce::ReqloomError> recordIfAbsent(
        const std::string& /*projectId*/, const std::filesystem::path& folder) override {
        ++calls;
        return folder;
    }
    std::expected<void, ce::ReqloomError> rebind(const std::string& /*projectId*/,
                                                 const std::filesystem::path& /*folder*/) override {
        ++calls;
        return {};
    }
    int calls{0};
};

struct Fakes {
    CapturingHttpClient* http{nullptr};
    FakeSecretStore* secrets{nullptr};
    CountingTrustStore* trust{nullptr};
};

ce::ExecutionEngine makeEngine(Fakes& out, const bool withSecretStore = true) {
    auto http = std::make_unique<CapturingHttpClient>();
    auto trust = std::make_unique<CountingTrustStore>();
    out.http = http.get();
    out.trust = trust.get();
    std::unique_ptr<ce::SecretStore> secrets{};
    if (withSecretStore) {
        auto store = std::make_unique<FakeSecretStore>();
        out.secrets = store.get();
        secrets = std::move(store);
    }
    ce::ExecutionEngine::Dependencies deps{
        std::move(http), nullptr, nullptr, std::move(secrets), nullptr};
    deps.trust = std::move(trust);
    return ce::ExecutionEngine{std::move(deps)};
}

/// One GET whose `X-Api-Key` header is `{{secret.<name>}}`.
ce::Project projectWithSecretHeader(const std::string& name = "API_KEY") {
    ce::Project project;
    project.name = "SecretEnv";
    project.id = "proj-env";
    project.rootDir = std::filesystem::temp_directory_path() / "reqloom-secret-env";
    project.defaultEnvironment = "local";
    project.environments["local"] = {{"baseUrl", "http://unused.test"}};

    ce::Operation op;
    op.id = ce::OperationId{"item.get"};
    op.resource = ce::ResourceId{"item"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/items";
    op.headers["X-Api-Key"] = "{{secret." + name + "}}";
    op.expectStatus = 200;

    ce::Resource res;
    res.id = ce::ResourceId{"item"};
    res.operations["get"] = std::move(op);
    project.resources[res.id] = std::move(res);
    return project;
}

[[nodiscard]] ce::Operation& onlyOperation(ce::Project& project) {
    return project.resources.begin()->second.operations.begin()->second;
}

/// What the CLI passes: environment secrets on, agent masking on.
[[nodiscard]] ce::RunOptions cliOptions() {
    ce::RunOptions options;
    options.secretsFromEnvironment = true;
    options.agentOutput = true;
    return options;
}

[[nodiscard]] const ce::StepResult* failedStep(const ce::RunResult& result) {
    for (const auto& step : result.steps) {
        if (step.status == ce::StepResult::Status::Failed) {
            return &step;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string sentHeader(const Fakes& fakes, const std::string& name) {
    if (fakes.http->requests.empty()) {
        return {};
    }
    const auto& headers = fakes.http->requests.front().headers;
    const auto it = headers.find(name);
    return it == headers.end() ? std::string{} : it->second;
}

constexpr const char* kNoBackend =
    "no secret backend available; set REQLOOM_SECRET_API_KEY or run on a desktop session with a "
    "keychain";

}  // namespace

// ─── Name mapping ────────────────────────────────────────────────────────────

TEST(SecretEnvName, maps_to_the_uppercased_name) {
    EXPECT_EQ(ce::secretEnvironmentVariable("API_KEY"), "REQLOOM_SECRET_API_KEY");
    EXPECT_EQ(ce::secretEnvironmentVariable("api_key"), "REQLOOM_SECRET_API_KEY");
    EXPECT_EQ(ce::secretEnvironmentVariable("stripe_Key2"), "REQLOOM_SECRET_STRIPE_KEY2");
    EXPECT_EQ(ce::secretEnvironmentVariable("1st"), "REQLOOM_SECRET_1ST");
}

TEST(SecretEnvName, refuses_a_name_it_cannot_map_instead_of_guessing) {
    EXPECT_FALSE(ce::secretEnvironmentVariable("api-key").has_value());
    EXPECT_FALSE(ce::secretEnvironmentVariable("a.b").has_value());
    EXPECT_FALSE(ce::secretEnvironmentVariable("my key").has_value());
    EXPECT_FALSE(ce::secretEnvironmentVariable("").has_value());
    EXPECT_FALSE(ce::secretEnvironmentVariable("caf\xc3\xa9").has_value());
}

// ─── Resolution order ────────────────────────────────────────────────────────

TEST(SecretEnvResolution, environment_variable_wins_and_the_keychain_is_not_read) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-env-value-1"};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->values["proj-env/API_KEY"] = "fake-keychain-value";

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-env-value-1");
    EXPECT_EQ(fakes.secrets->reads, 0);
    EXPECT_EQ(fakes.trust->calls, 0) << "an environment value needs no keychain trust check";
}

TEST(SecretEnvResolution, keychain_is_used_when_the_variable_is_absent) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->values["proj-env/API_KEY"] = "fake-keychain-value";

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-keychain-value");
}

TEST(SecretEnvResolution, works_with_no_secret_store_and_no_project_id) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-env-value-2"};
    Fakes fakes;
    auto engine = makeEngine(fakes, /*withSecretStore=*/false);

    auto project = projectWithSecretHeader();
    project.id.clear();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-env-value-2");
}

TEST(SecretEnvResolution, bare_name_variable_is_never_read) {
    const ScopedEnv bare{"API_KEY", "fake-bare-value"};
    const ScopedEnv prefixed{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
}

TEST(SecretEnvResolution, empty_variable_counts_as_unset_and_says_so) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", ""};
    Fakes fakes;
    auto engine = makeEngine(fakes);

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
#ifndef _WIN32
    // Windows cannot hold an empty variable: setting one to "" removes it.
    EXPECT_NE(step->detail.find("REQLOOM_SECRET_API_KEY is set but empty"), std::string::npos)
        << step->detail;
#endif
}

TEST(SecretEnvResolution, environment_value_is_masked_like_any_secret) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-env-value-masked"};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    std::vector<std::string> headerValues;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* prepared = std::get_if<ce::RequestPrepared>(&event)) {
            for (const auto& [name, value] : prepared->maskedHeaders) {
                headerValues.push_back(value);
            }
        }
    });

    auto project = projectWithSecretHeader();
    onlyOperation(project).headers.clear();
    onlyOperation(project).headers["X-Custom"] = "{{secret.API_KEY}}";
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_FALSE(headerValues.empty());
    for (const auto& value : headerValues) {
        EXPECT_EQ(value.find("fake-env-value-masked"), std::string::npos) << value;
    }
}

// ─── No backend: say so on the first unresolved secret ───────────────────────

TEST(SecretEnvResolution, no_backend_names_the_variable_to_set) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
    EXPECT_NE(step->detail.find("{{secret.API_KEY}}"), std::string::npos) << step->detail;
    EXPECT_EQ(fakes.trust->calls, 0) << "the trust check must not pre-empt the message";
}

TEST(SecretEnvResolution, engine_without_a_secret_store_says_no_backend) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes, /*withSecretStore=*/false);

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, keychain_found_unusable_while_reading_says_no_backend) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->unreachableReads = 1;

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, keychain_without_the_secret_names_the_variable_too) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("REQLOOM_SECRET_API_KEY"), std::string::npos) << step->detail;
    EXPECT_EQ(step->detail.find("no secret backend"), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, keychain_read_failure_points_at_the_variable) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->fail = true;

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SecretAccessFailed);
    EXPECT_NE(result.error().detail.find("REQLOOM_SECRET_API_KEY"), std::string::npos)
        << result.error().detail;
}

TEST(SecretEnvResolution, unmappable_name_without_a_backend_says_it_cannot_come_from_the_env) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader("api-key");
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("cannot be supplied through an environment variable"),
              std::string::npos)
        << step->detail;
}

TEST(SecretEnvResolution, unmappable_name_still_resolves_from_the_keychain) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->values["proj-env/api-key"] = "fake-keychain-dash";

    auto project = projectWithSecretHeader("api-key");
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-keychain-dash");
}

// ─── Every place a secret can be used fails before sending ───────────────────

TEST(SecretEnvResolution, inline_bearer_token_without_a_backend_sends_nothing) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    auto& op = onlyOperation(project);
    op.headers.clear();
    ce::InlineAuth auth;
    auth.type = ce::InlineAuthType::Bearer;
    auth.token = "{{secret.API_KEY}}";
    op.inlineAuth = auth;
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, actor_login_body_without_a_backend_sends_nothing) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    ce::Actor actor;
    actor.id = ce::ActorId{"user"};
    actor.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep login;
    login.id = "login";
    login.pathTemplate = "/login";
    login.bodyTemplate = R"({"password":"{{secret.API_KEY}}"})";
    login.expectStatus = 200;
    actor.authSteps.push_back(login);
    project.actors[actor.id] = actor;
    auto& op = onlyOperation(project);
    op.headers.clear();
    op.actor = actor.id;
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, basic_auth_config_without_a_backend_sends_nothing) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    ce::Actor actor;
    actor.id = ce::ActorId{"svc"};
    actor.strategy = ce::AuthStrategy::Basic;
    actor.authConfig["username"] = "svc";
    actor.authConfig["password"] = "{{secret.API_KEY}}";
    project.actors[actor.id] = actor;
    auto& op = onlyOperation(project);
    op.headers.clear();
    op.actor = actor.id;
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(step->detail.find(kNoBackend), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, secret_tagged_environment_value_without_a_backend_sends_nothing) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", std::nullopt};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    // `api_key: !secret API_KEY` in an environment file parses to this.
    project.environments["local"]["api_key"] = "{{secret.API_KEY}}";
    auto& op = onlyOperation(project);
    op.headers.clear();
    op.headers["X-Api-Key"] = "{{env.api_key}}";
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(step->detail.find("REQLOOM_SECRET_API_KEY"), std::string::npos) << step->detail;
}

// ─── Owner decisions: CLI only, uppercase, no shared variable ───────────────

TEST(SecretEnvResolution, desktop_runs_never_read_the_environment) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-env-must-not-win"};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->values["proj-env/API_KEY"] = "fake-keychain-value";

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-keychain-value");
}

TEST(SecretEnvResolution, desktop_message_without_a_backend_does_not_offer_the_variable_as_a_fix) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-env-value-3"};
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = projectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests.empty()) << "the desktop must not use the variable";
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_NE(step->detail.find("no secret backend available"), std::string::npos) << step->detail;
    EXPECT_NE(step->detail.find("reqloom CLI"), std::string::npos) << step->detail;
}

TEST(SecretEnvResolution, lowercase_name_reads_the_uppercased_variable_only) {
    const ScopedEnv upper{"REQLOOM_SECRET_API_KEY", "fake-upper-value"};
    const ScopedEnv lower{"REQLOOM_SECRET_api_key", "fake-lower-value"};
    Fakes fakes;
    auto engine = makeEngine(fakes);

    auto project = projectWithSecretHeader("api_key");
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-upper-value");
}

TEST(SecretEnvResolution, two_names_sharing_one_variable_fail_the_run_before_sending) {
    const ScopedEnv env{"REQLOOM_SECRET_API_KEY", "fake-shared-value"};
    Fakes fakes;
    auto engine = makeEngine(fakes);

    auto project = projectWithSecretHeader("api_key");
    onlyOperation(project).headers["X-Other"] = "{{secret.API_KEY}}";
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, cliOptions());

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(fakes.http->requests.empty());
    EXPECT_NE(result.error().detail.find("REQLOOM_SECRET_API_KEY"), std::string::npos)
        << result.error().detail;
    EXPECT_NE(result.error().detail.find("api_key"), std::string::npos) << result.error().detail;
}

TEST(SecretEnvResolution, a_keychain_that_was_unreachable_is_tried_again_next_run) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->values["proj-env/API_KEY"] = "fake-keychain-later";
    fakes.secrets->unreachableReads = 1;

    auto project = projectWithSecretHeader();
    ce::RunContext first;
    const auto failed = engine.run(project, ce::OperationId{"item.get"}, first);
    ASSERT_TRUE(failed.has_value()) << failed.error().detail;
    EXPECT_FALSE(failed->succeeded());

    ce::RunContext second;
    const auto ok = engine.run(project, ce::OperationId{"item.get"}, second);
    ASSERT_TRUE(ok.has_value()) << ok.error().detail;
    EXPECT_TRUE(ok->succeeded());
    EXPECT_EQ(sentHeader(fakes, "X-Api-Key"), "fake-keychain-later");
}
