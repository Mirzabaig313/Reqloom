// JsonBodyEscapingTest — the request body actually sent escapes placeholder
// values that sit inside JSON string literals, for operation bodies and
// actor auth step bodies, while values outside strings stay raw.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
namespace fs = std::filesystem;

namespace {

class CapturingHttpClient final : public ce::HttpClient {
public:
    void enqueue(const int status, std::string body) {
        ce::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        responses_.push_back(std::move(response));
    }

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests_.push_back(request);
        if (responseIndex_ >= responses_.size()) {
            ce::HttpResponse response;
            response.status = 500;
            return response;
        }
        return responses_[responseIndex_++];
    }

    [[nodiscard]] const std::vector<ce::HttpRequest>& requests() const noexcept {
        return requests_;
    }

private:
    std::vector<ce::HttpRequest> requests_;
    std::vector<ce::HttpResponse> responses_;
    std::size_t responseIndex_{};
};

class ScratchProject {
public:
    explicit ScratchProject(const std::string& yamlBody) {
        path_ = ct::uniqueTempPath("reqloom-jsonbody-itest");
        fs::create_directories(path_);
        std::ofstream{path_ / "reqloom.yaml"} << yamlBody;
    }

    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;
    ScratchProject(ScratchProject&&) = delete;
    ScratchProject& operator=(ScratchProject&&) = delete;

    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] fs::path yaml() const { return path_ / "reqloom.yaml"; }

private:
    fs::path path_;
};

constexpr const char* kProjectYaml = R"YAML(
version: 1
name: JsonBodyEscaping
default_environment: local

environment:
  baseUrl: http://json-body.test
  TITLE: 'x","role":"admin'
  PASSWORD: 'p"w\d'
  COUNT: '3'
  TAGS: '["a","b"]'

actors:
  user:
    auth:
      method: POST
      path: /login
      body: '{"email": "u@example.test", "password": "{{env.PASSWORD}}"}'
      extract: { token: $.token }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  post:
    operations:
      create:
        method: POST
        path: /posts
        actor: user
        body: '{"note": "{{env.TITLE}}", "count": {{env.COUNT}}, "tags": {{env.TAGS}}}'
        expect_status: 201
)YAML";

}  // namespace

TEST(JsonBodyEscaping, sent_bodies_escape_string_placeholders_and_keep_raw_values) {
    ScratchProject project(kProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;

    auto http = std::make_unique<CapturingHttpClient>();
    auto* captured = http.get();
    captured->enqueue(200, R"({"token":"tok"})");
    captured->enqueue(201, "{}");
    ce::ExecutionEngine::Dependencies dependencies{
        std::move(http), nullptr, nullptr, nullptr, nullptr};
    dependencies.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(dependencies)};

    ce::RunContext ctx;
    const auto result = engine.run(*loaded, ce::OperationId{"post.create"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded());

    ASSERT_EQ(captured->requests().size(), 2u);
    const auto& login = captured->requests()[0];
    ASSERT_TRUE(login.body.has_value());
    EXPECT_EQ(*login.body, R"({"email": "u@example.test", "password": "p\"w\\d"})");
    EXPECT_EQ(nlohmann::json::parse(*login.body)["password"], R"(p"w\d)");

    const auto& create = captured->requests()[1];
    ASSERT_TRUE(create.body.has_value());
    EXPECT_EQ(*create.body, R"({"note": "x\",\"role\":\"admin", "count": 3, "tags": ["a","b"]})");
    const auto sent = nlohmann::json::parse(*create.body);
    EXPECT_FALSE(sent.contains("role"));
    EXPECT_EQ(sent["note"], R"(x","role":"admin)");
    EXPECT_EQ(sent["count"], 3);
    EXPECT_EQ(sent["tags"].size(), 2u);
}
