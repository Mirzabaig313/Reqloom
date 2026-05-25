// Unit tests for the YAML schema parser's polling and multi-status

//   - poll_until block is silently dropped → pollUntil stays nullopt
//   - expect_status: [200, 202] → as<int>() throws inside YamlSchemaParser
#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

/// Per-test scratch directory under the system temp dir. Cleaned up
/// in the destructor — keeps the engine tree free of test artefacts
/// even when an assertion fails partway through.
class ScratchDir {
public:
    ScratchDir() {
        const auto unique =
            "chainapi-schema-poll-" + std::to_string(::getpid()) +
            "-" + std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

    fs::path write(const std::string& filename, const std::string& body) {
        const auto full = path_ / filename;
        std::ofstream out{full};
        out << body;
        return full;
    }

private:
    fs::path path_;
    inline static int counter_{0};
};

}  // namespace

TEST(SchemaParserPolling, parses_poll_until_block_with_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: PollSample
environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /login
      body: { email: "a@b.test" }
      extract: { token: $.token }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  payment:
    operations:
      pay:
        method: POST
        path: /api/v1/pay
        actor: user
        expect_status: [200, 202]
        body: { method: "card" }
        poll_until:
          method: GET
          path: /api/v1/pay/{{payment.payment_id}}/status
          success_when: "$.status == 'COMPLETED'"
          fail_when:    "$.status in ['FAILED', 'CANCELLED']"
          interval: 500ms
          timeout: 30s
          max_attempts: 60
        extract:
          payment_id: $.id
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& payment = result->resources.at(ce::ResourceId{"payment"});
    const auto& pay = payment.operations.at("pay");

    ASSERT_TRUE(pay.pollUntil.has_value()) << "poll_until should be parsed";
    EXPECT_EQ(pay.pollUntil->method, ce::HttpMethod::Get);
    EXPECT_EQ(pay.pollUntil->pathTemplate,
              "/api/v1/pay/{{payment.payment_id}}/status");
    EXPECT_EQ(pay.pollUntil->successWhen, "$.status == 'COMPLETED'");
    ASSERT_TRUE(pay.pollUntil->failWhen.has_value());
    EXPECT_EQ(*pay.pollUntil->failWhen, "$.status in ['FAILED', 'CANCELLED']");
    EXPECT_EQ(pay.pollUntil->interval, std::chrono::milliseconds{500});
    EXPECT_EQ(pay.pollUntil->timeout,  std::chrono::milliseconds{30'000});
    EXPECT_EQ(pay.pollUntil->maxAttempts, 60);

    // expect_status: [200, 202] populates the multi-value list, not the
    // singular field. The executor consults the list when non-empty.
    EXPECT_EQ(pay.expectStatusList,
              std::vector<int>({200, 202}));
    EXPECT_FALSE(pay.expectStatus.has_value());
}

TEST(SchemaParserPolling, parses_poll_until_with_backoff_and_actor_override) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: PollSample
environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }
  admin:
    auth: { method: POST, path: /admin/login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{admin.token}}" } }

resources:
  job:
    operations:
      submit:
        method: POST
        path: /api/v1/jobs
        actor: user
        expect_status: 202
        poll_until:
          method: GET
          path: /api/v1/jobs/{{job.job_id}}
          actor: admin
          success_when: "$.state == 'done'"
          backoff:
            base: 100ms
            max:  5s
          timeout: 1m
        extract: { job_id: $.id }
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& submit =
        result->resources.at(ce::ResourceId{"job"}).operations.at("submit");

    ASSERT_TRUE(submit.pollUntil.has_value());
    ASSERT_TRUE(submit.pollUntil->actor.has_value());
    EXPECT_EQ(submit.pollUntil->actor->value, "admin");
    ASSERT_TRUE(submit.pollUntil->backoffBase.has_value());
    EXPECT_EQ(*submit.pollUntil->backoffBase, std::chrono::milliseconds{100});
    EXPECT_EQ(submit.pollUntil->backoffMax, std::chrono::milliseconds{5'000});
    EXPECT_EQ(submit.pollUntil->timeout,    std::chrono::milliseconds{60'000});

    // Singular expect_status form remains supported for non-polling ops.
    EXPECT_EQ(submit.expectStatus.value_or(0), 202);
    EXPECT_TRUE(submit.expectStatusList.empty());
}

TEST(SchemaParserPolling, ops_without_poll_until_remain_nullopt) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: NoPolling
environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  product:
    operations:
      get:
        method: GET
        path: /api/v1/products/123
        actor: user
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& get =
        result->resources.at(ce::ResourceId{"product"}).operations.at("get");

    EXPECT_FALSE(get.pollUntil.has_value());
    EXPECT_TRUE(get.expectStatusList.empty());
    EXPECT_EQ(get.expectStatus.value_or(0), 200);
}


// ─── Slice 4b — Basic auth strategy ─────────────────────────────────────────

TEST(SchemaParserBasicAuth, parses_basic_strategy_into_authConfig) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: BasicSample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  client:
    auth:
      strategy: basic
      username: "Aladdin"
      password: "open sesame"
    inject:
      headers: { Authorization: "Basic {{client.credential}}" }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: client
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& client = result->actors.at(ce::ActorId{"client"});
    EXPECT_EQ(client.strategy, ce::AuthStrategy::Basic);
    EXPECT_TRUE(client.authSteps.empty());
    EXPECT_EQ(client.authConfig.at("username"), "Aladdin");
    EXPECT_EQ(client.authConfig.at("password"), "open sesame");
    EXPECT_EQ(client.inject.headers.at("Authorization"),
              "Basic {{client.credential}}");
}


// ─── Slice 4c — api_key auth strategy ───────────────────────────────────────

TEST(SchemaParserApiKeyAuth, parses_api_key_strategy_with_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ApiKeySample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  service:
    auth:
      strategy: api_key
      key: "{{secret.SERVICE_API_KEY}}"
      location: header
      name: X-API-Key

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: service
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& service = result->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(service.strategy, ce::AuthStrategy::ApiKey);
    EXPECT_TRUE(service.authSteps.empty());
    EXPECT_EQ(service.authConfig.at("key"), "{{secret.SERVICE_API_KEY}}");
    EXPECT_EQ(service.authConfig.at("location"), "header");
    EXPECT_EQ(service.authConfig.at("name"), "X-API-Key");
}

TEST(SchemaParserApiKeyAuth, parses_api_key_with_only_required_key) {
    // Manual-inject form: just `key`, no `location`/`name`.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ApiKeyManualSample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  service:
    auth:
      strategy: api_key
      key: "sk_live_abc"
    inject:
      headers: { Authorization: "Bearer {{service.key}}" }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: service
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& service = result->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(service.strategy, ce::AuthStrategy::ApiKey);
    EXPECT_EQ(service.authConfig.at("key"), "sk_live_abc");
    EXPECT_FALSE(service.authConfig.contains("location"));
    EXPECT_FALSE(service.authConfig.contains("name"));
    EXPECT_EQ(service.inject.headers.at("Authorization"),
              "Bearer {{service.key}}");
}


// ─── Slice 4d — oauth2_client_credentials auth strategy ─────────────────────

TEST(SchemaParserOAuth2ClientCreds, parses_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OAuth2Sample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  service:
    auth:
      strategy: oauth2_client_credentials
      token_url: "{{env.baseUrl}}/oauth/token"
      client_id: "{{secret.OAUTH_CLIENT_ID}}"
      client_secret: "{{secret.OAUTH_CLIENT_SECRET}}"
      scope: "read:orders write:orders"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: service
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& service = result->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(service.strategy, ce::AuthStrategy::OAuth2ClientCredentials);
    EXPECT_TRUE(service.authSteps.empty());
    EXPECT_EQ(service.authConfig.at("token_url"),
              "{{env.baseUrl}}/oauth/token");
    EXPECT_EQ(service.authConfig.at("client_id"),
              "{{secret.OAUTH_CLIENT_ID}}");
    EXPECT_EQ(service.authConfig.at("client_secret"),
              "{{secret.OAUTH_CLIENT_SECRET}}");
    EXPECT_EQ(service.authConfig.at("scope"),
              "read:orders write:orders");
}

TEST(SchemaParserOAuth2ClientCreds, scope_is_optional) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OAuth2NoScope
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  service:
    auth:
      strategy: oauth2_client_credentials
      token_url: "https://idp.test/oauth/token"
      client_id: "id"
      client_secret: "sec"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: service
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& service = result->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(service.strategy, ce::AuthStrategy::OAuth2ClientCredentials);
    EXPECT_FALSE(service.authConfig.contains("scope"));
}


// ─── Slice 4e — oauth2_password auth strategy ───────────────────────────────

TEST(SchemaParserOAuth2Password, parses_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OAuth2PasswordSample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      strategy: oauth2_password
      token_url: "{{env.baseUrl}}/oauth/token"
      client_id: "{{secret.OAUTH_CLIENT_ID}}"
      client_secret: "{{secret.OAUTH_CLIENT_SECRET}}"
      username: "alice@example.com"
      password: "{{secret.RO_PASS}}"
      scope: "read:notes"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: user
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& user = result->actors.at(ce::ActorId{"user"});
    EXPECT_EQ(user.strategy, ce::AuthStrategy::OAuth2Password);
    EXPECT_TRUE(user.authSteps.empty());
    EXPECT_EQ(user.authConfig.at("token_url"),
              "{{env.baseUrl}}/oauth/token");
    EXPECT_EQ(user.authConfig.at("username"), "alice@example.com");
    EXPECT_EQ(user.authConfig.at("password"), "{{secret.RO_PASS}}");
    EXPECT_EQ(user.authConfig.at("scope"),    "read:notes");
}
