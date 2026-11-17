// SecretLoadingTest — verifies the SecretStore → ResolveContext bridge.
//
// At run start the engine reads every `{{secret.X}}` the project
// references from the injected SecretStore into the resolve context, so
// the value substitutes into outbound requests. Covers:
//   - a referenced secret resolves into a request header
//   - a backend read failure aborts the run with SecretAccessFailed
//   - a missing key leaves the reference unresolved (no crash)
//
// Uses fakes for all dependencies (no network, no keychain) so the test
// is hermetic and proves the wiring rather than any concrete backend.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;

namespace {

/// Records each outbound request and replays a canned 200 response.
class CapturingHttpClient final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& req) override {
        requests_.push_back(req);
        ce::HttpResponse resp;
        resp.status = 200;
        resp.body = R"({"id":"item-1"})";
        return resp;
    }

    [[nodiscard]] const std::vector<ce::HttpRequest>& requests() const noexcept {
        return requests_;
    }

private:
    std::vector<ce::HttpRequest> requests_;
};

/// In-memory SecretStore. `put` seeds a value; `failOn` forces a backend
/// error for one key to exercise the SecretAccessFailed path.
class FakeSecretStore final : public ce::SecretStore {
public:
    void put(std::string name, std::string value) { values_[std::move(name)] = std::move(value); }
    void failOn(std::string name) { failKey_ = std::move(name); }

    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        ++reads;
        if (name == failKey_) {
            return std::unexpected(ce::ReqloomError{
                ce::ErrorCode::SecretAccessFailed, ce::ErrorClass::Auth, "fake backend failure"});
        }
        if (auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }

    std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                const std::string& value) override {
        values_[name] = value;
        return {};
    }

    std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

    [[nodiscard]] bool available() const noexcept override { return backendPresent; }

    int reads{0};
    bool backendPresent{true};

private:
    std::map<std::string, std::string> values_;
    std::string failKey_;
};

/// In-memory ProjectTrustStore: the test seam standing in for the per-user
/// file. `calls` counts every access so a test can prove it was not touched.
class FakeTrustStore final : public ce::ProjectTrustStore {
public:
    std::expected<std::optional<std::filesystem::path>, ce::ReqloomError> folderFor(
        const std::string& projectId) override {
        ++calls;
        if (auto it = folders.find(projectId); it != folders.end()) {
            return std::optional<std::filesystem::path>{it->second};
        }
        return std::optional<std::filesystem::path>{};
    }

    std::expected<std::filesystem::path, ce::ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) override {
        ++calls;
        return folders.try_emplace(projectId, folder).first->second;
    }

    std::expected<void, ce::ReqloomError> rebind(const std::string& projectId,
                                                 const std::filesystem::path& folder) override {
        ++calls;
        folders[projectId] = folder;
        return {};
    }

    std::map<std::string, std::filesystem::path> folders;
    int calls{0};
};

/// Folder the test projects claim to live in. Never created: the folder
/// check resolves symlinks in the existing part of the path only.
std::filesystem::path projectFolder(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("reqloom-secret-loading-" + name);
}

/// A one-operation project whose request header references {{secret.API_KEY}}.
/// The actor is empty so no auth flow runs — the test isolates secret
/// resolution on the main request path.
ce::Project makeProjectWithSecretHeader() {
    ce::Project project;
    project.name = "SecretLoadingTest";
    project.id = "proj-test";
    project.rootDir = projectFolder("a");
    project.defaultEnvironment = "local";
    project.environments["local"] = {{"baseUrl", "http://unused.test"}};

    ce::Operation op;
    op.id = ce::OperationId{"item.get"};
    op.resource = ce::ResourceId{"item"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/api/v1/items";
    op.headers["X-Api-Key"] = "{{secret.API_KEY}}";
    op.expectStatus = 200;

    ce::Resource res;
    res.id = ce::ResourceId{"item"};
    res.operations["get"] = std::move(op);
    project.resources[res.id] = std::move(res);
    return project;
}

struct Fakes {
    CapturingHttpClient* http{nullptr};
    FakeSecretStore* secrets{nullptr};
    FakeTrustStore* trust{nullptr};
};

/// Build an engine with capturing-http + fake-secrets and null schema/
/// history/hooks (unused by a pre-parsed run with no scripts).
ce::ExecutionEngine makeEngine(Fakes& out) {
    auto http = std::make_unique<CapturingHttpClient>();
    auto secrets = std::make_unique<FakeSecretStore>();
    auto trust = std::make_unique<FakeTrustStore>();
    out.http = http.get();
    out.secrets = secrets.get();
    out.trust = trust.get();
    ce::ExecutionEngine::Dependencies deps{
        std::move(http), nullptr, nullptr, std::move(secrets), nullptr};
    deps.trust = std::move(trust);
    return ce::ExecutionEngine{std::move(deps)};
}

}  // namespace

TEST(SecretLoading, referenced_secret_resolves_into_request_header) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_FALSE(fakes.http->requests().empty());

    const auto& headers = fakes.http->requests().front().headers;
    auto it = headers.find("X-Api-Key");
    ASSERT_NE(it, headers.end());
    EXPECT_EQ(it->second, "sk_live_zzz");
}

TEST(SecretLoading, backend_failure_aborts_run_with_secret_access_failed) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->failOn("proj-test/API_KEY");

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SecretAccessFailed);
    // The run must abort before any request goes out.
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, missing_secret_leaves_reference_unresolved_without_crashing) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    // API_KEY is never seeded — read() returns nullopt, not an error.

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    // The run itself completes (no schema-time error); the unresolved
    // reference is a runtime concern handled on the request path.
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

[[nodiscard]] const ce::StepResult* failedStep(const ce::RunResult& result) {
    for (const auto& step : result.steps) {
        if (step.status == ce::StepResult::Status::Failed) {
            return &step;
        }
    }
    return nullptr;
}

TEST(SecretLoading, secret_without_a_keychain_says_no_keychain_is_available) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->backendPresent = false;

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    std::vector<std::string> failures;
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&event)) {
            failures.push_back(failed->detail);
        }
    });
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests().empty());
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    // FR-11.6: the cause up front. This engine reads no environment (desktop), so the message
    // points at a keychain or the CLI rather than offering the variable as a fix here.
    const std::string expected = "no secret backend available";
    EXPECT_NE(step->detail.find(expected), std::string::npos) << step->detail;
    EXPECT_NE(step->detail.find("{{secret.API_KEY}}"), std::string::npos) << step->detail;
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_NE(failures.front().find(expected), std::string::npos) << failures.front();
}

TEST(SecretLoading, missing_secret_with_a_keychain_does_not_blame_the_keychain) {
    Fakes fakes;
    auto engine = makeEngine(fakes);

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto* step = failedStep(*result);
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->detail.find("keychain"), std::string::npos) << step->detail;
}

TEST(SecretLoading, dry_run_does_not_touch_the_secret_store) {
    // A dry run previews the resolved chain without sending requests, so it
    // must not hit the keychain (which can pop an interactive unlock
    // prompt). Even a backend that would fail on read must not be consulted.
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->failOn("proj-test/API_KEY");

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    ce::RunOptions options;
    options.dryRun = true;
    auto result = engine.run(project, ce::OperationId{"item.get"}, ctx, options);

    // No SecretAccessFailed: the store was never read. Dry run succeeds.
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(fakes.http->requests().empty());
    EXPECT_EQ(fakes.trust->calls, 0) << "a dry run must not touch the project trust store";
}

TEST(SecretLoading, first_run_binds_the_id_to_the_projects_folder) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    ASSERT_TRUE(engine.run(project, ce::OperationId{"item.get"}, ctx).has_value());

    ASSERT_TRUE(fakes.trust->folders.contains("proj-test"));
    EXPECT_EQ(fakes.trust->folders.at("proj-test"),
              std::filesystem::weakly_canonical(projectFolder("a")));
}

TEST(SecretLoading, secrets_load_for_the_bound_folder) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");
    fakes.trust->folders["proj-test"] = std::filesystem::weakly_canonical(projectFolder("a"));

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(fakes.http->requests().size(), 1u);
    EXPECT_EQ(fakes.http->requests().front().headers.at("X-Api-Key"), "sk_live_zzz");
}

TEST(SecretLoading, second_folder_claiming_a_bound_id_is_refused_before_any_request) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");
    const auto owner = std::filesystem::weakly_canonical(projectFolder("a"));
    fakes.trust->folders["proj-test"] = owner;

    auto impostor = makeProjectWithSecretHeader();
    impostor.rootDir = projectFolder("b");
    ce::RunContext ctx;
    const auto result = engine.run(impostor, ce::OperationId{"item.get"}, ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SecretAccessFailed);
    EXPECT_NE(result.error().detail.find(owner.string()), std::string::npos)
        << result.error().detail;
    EXPECT_NE(
        result.error().detail.find(std::filesystem::weakly_canonical(projectFolder("b")).string()),
        std::string::npos)
        << result.error().detail;
    EXPECT_EQ(fakes.secrets->reads, 0) << "no secret may be read for the impostor";
    EXPECT_TRUE(fakes.http->requests().empty());
    EXPECT_EQ(fakes.trust->folders.at("proj-test"), owner) << "the record must not move";
}

TEST(SecretLoading, in_memory_project_without_a_folder_cannot_use_secrets) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    project.rootDir.clear();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SecretAccessFailed);
    EXPECT_EQ(fakes.secrets->reads, 0);
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, engine_without_a_trust_store_refuses_secrets) {
    auto http = std::make_unique<CapturingHttpClient>();
    auto* httpRaw = http.get();
    auto secrets = std::make_unique<FakeSecretStore>();
    secrets->put("proj-test/API_KEY", "sk_live_zzz");
    ce::ExecutionEngine engine{ce::ExecutionEngine::Dependencies{
        std::move(http), nullptr, nullptr, std::move(secrets), nullptr}};

    auto project = makeProjectWithSecretHeader();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SecretAccessFailed);
    EXPECT_TRUE(httpRaw->requests().empty());
}

TEST(SecretLoading, raw_query_templates_are_safe_and_structured_query_precedes_fragment) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "ok&admin=true#");

    auto project = makeProjectWithSecretHeader();
    auto& operation = project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    operation.pathTemplate = "/search?x={{secret.API_KEY}}&x=fixed#results";
    operation.queryParams["next"] = "a&b";

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(fakes.http->requests().size(), 1u);
    EXPECT_EQ(fakes.http->requests().front().url,
              "http://unused.test/search?x=ok%26admin%3Dtrue%23&x=fixed&next=a%26b#results");
}

TEST(SecretLoading, whole_path_reference_cannot_change_request_authority) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    project.environments["local"]["path"] = "@evil.test/collect";
    auto& operation = project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    operation.pathTemplate = "{{env.path}}";

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().status, ce::StepResult::Status::Failed);
    EXPECT_EQ(result->steps.front().error.value_or(ce::ErrorCode::Internal),
              ce::ErrorCode::VarUnresolved);
    EXPECT_TRUE(fakes.http->requests().empty())
        << "a whole path reference must not change the baseUrl authority or dispatch X-Api-Key";
}

TEST(SecretLoading, missing_query_parameter_reference_fails_before_request) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    auto& operation = project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    operation.queryParams["next"] = "{{env.missing}}";

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().status, ce::StepResult::Status::Failed);
    EXPECT_EQ(result->steps.front().error.value_or(ce::ErrorCode::Internal),
              ce::ErrorCode::VarUnresolved);
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, projects_with_different_ids_read_their_own_secret_values) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-a/API_KEY", "value-a");
    fakes.secrets->put("proj-b/API_KEY", "value-b");

    auto projectA = makeProjectWithSecretHeader();
    projectA.id = "proj-a";
    auto projectB = makeProjectWithSecretHeader();
    projectB.id = "proj-b";

    ce::RunContext ctxA;
    ASSERT_TRUE(engine.run(projectA, ce::OperationId{"item.get"}, ctxA).has_value());
    ce::RunContext ctxB;
    ASSERT_TRUE(engine.run(projectB, ce::OperationId{"item.get"}, ctxB).has_value());

    ASSERT_EQ(fakes.http->requests().size(), 2u);
    EXPECT_EQ(fakes.http->requests()[0].headers.at("X-Api-Key"), "value-a");
    EXPECT_EQ(fakes.http->requests()[1].headers.at("X-Api-Key"), "value-b");
}

TEST(SecretLoading, project_cannot_read_another_projects_secret_or_a_bare_key) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-a/API_KEY", "value-a");
    fakes.secrets->put("API_KEY", "bare-value");

    auto projectB = makeProjectWithSecretHeader();
    projectB.id = "proj-b";
    ce::RunContext ctx;
    const auto result = engine.run(projectB, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().error.value_or(ce::ErrorCode::Internal),
              ce::ErrorCode::VarUnresolved);
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, project_without_id_that_uses_secrets_fails_before_any_request) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("API_KEY", "bare-value");

    auto project = makeProjectWithSecretHeader();
    project.id.clear();
    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(result.error().detail.find("id: "), std::string::npos) << result.error().detail;
    EXPECT_NE(result.error().detail.find("reqloom.yaml"), std::string::npos)
        << result.error().detail;
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, env_builtin_not_in_allowed_env_fails_with_actionable_detail) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");

    auto project = makeProjectWithSecretHeader();
    auto& operation = project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    operation.headers["X-Build"] = "{{$.env.PATH}}";

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->steps.size(), 1u);
    EXPECT_EQ(result->steps.front().error.value_or(ce::ErrorCode::Internal),
              ce::ErrorCode::VarUnresolved);
    EXPECT_NE(result->steps.front().detail.find(
                  "environment variable PATH is not in allowed_env in reqloom.yaml"),
              std::string::npos)
        << result->steps.front().detail;
    EXPECT_TRUE(fakes.http->requests().empty());
}

TEST(SecretLoading, env_builtin_listed_in_allowed_env_resolves_into_request) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("proj-test/API_KEY", "sk_live_zzz");
    const char* path = std::getenv("PATH");
    ASSERT_NE(path, nullptr);

    auto project = makeProjectWithSecretHeader();
    project.allowedEnv = {"PATH"};
    auto& operation = project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    operation.headers["X-Build"] = "{{$.env.PATH}}";

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(fakes.http->requests().size(), 1u);
    EXPECT_EQ(fakes.http->requests().front().headers.at("X-Build"), std::string{path});
}
