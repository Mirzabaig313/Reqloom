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

#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ce = chainapi::engine;

namespace {

/// Records each outbound request and replays a canned 200 response.
class CapturingHttpClient final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ChainApiError> send(const ce::HttpRequest& req) override {
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

    std::expected<std::optional<std::string>, ce::ChainApiError> read(
        const std::string& name) override {
        if (name == failKey_) {
            return std::unexpected(ce::ChainApiError{
                ce::ErrorCode::SecretAccessFailed, ce::ErrorClass::Auth, "fake backend failure"});
        }
        if (auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }

    std::expected<void, ce::ChainApiError> write(const std::string& name,
                                                 const std::string& value) override {
        values_[name] = value;
        return {};
    }

    std::expected<void, ce::ChainApiError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

private:
    std::map<std::string, std::string> values_;
    std::string failKey_;
};

/// A one-operation project whose request header references {{secret.API_KEY}}.
/// The actor is empty so no auth flow runs — the test isolates secret
/// resolution on the main request path.
ce::Project makeProjectWithSecretHeader() {
    ce::Project project;
    project.name = "SecretLoadingTest";
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
};

/// Build an engine with capturing-http + fake-secrets and null schema/
/// history/hooks (unused by a pre-parsed run with no scripts).
ce::ExecutionEngine makeEngine(Fakes& out) {
    auto http = std::make_unique<CapturingHttpClient>();
    auto secrets = std::make_unique<FakeSecretStore>();
    out.http = http.get();
    out.secrets = secrets.get();
    ce::ExecutionEngine::Dependencies deps{
        std::move(http), nullptr, nullptr, std::move(secrets), nullptr};
    return ce::ExecutionEngine{std::move(deps)};
}

}  // namespace

TEST(SecretLoading, referenced_secret_resolves_into_request_header) {
    Fakes fakes;
    auto engine = makeEngine(fakes);
    fakes.secrets->put("API_KEY", "sk_live_zzz");

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
    fakes.secrets->failOn("API_KEY");

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
