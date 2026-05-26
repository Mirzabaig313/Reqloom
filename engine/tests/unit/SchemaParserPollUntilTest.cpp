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


// ─── Slice 5a — sibling-file hooks ──────────────────────────────────────────
//
// PRD §5.10: hook scripts can live in a sibling `.js` file referenced
// by relative path. The parser detects path-shaped values, validates
// containment under the project root, caps file size at 1 MiB, and
// loads the content into Operation::preRequestScript /
// postResponseScript. Inline JS continues to work — anything that
// doesn't look like a path falls through unchanged.

TEST(SchemaParserHooks, loads_sibling_js_file_for_pre_request) {
    ScratchDir scratch;
    // Write the hook file first so the parser can resolve it.
    std::ofstream hookOut{scratch.path() / "hooks-sign.js"};
    hookOut << "export default function (ctx) {\n"
            << "  ctx.request.headers['X-Hook'] = 'fired';\n"
            << "}\n";
    hookOut.close();

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookFromFile
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  signed:
    operations:
      send:
        method: POST
        path: /api/v1/signed
        actor: user
        pre_request: ./hooks-sign.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& send = result->resources.at(ce::ResourceId{"signed"})
                            .operations.at("send");
    ASSERT_TRUE(send.preRequestScript.has_value());
    EXPECT_NE(send.preRequestScript->find("X-Hook"), std::string::npos)
        << "expected file content to land in preRequestScript; got: "
        << *send.preRequestScript;
    EXPECT_NE(send.preRequestScript->find("export default"),
              std::string::npos);
}

TEST(SchemaParserHooks, loads_post_response_from_file_too) {
    ScratchDir scratch;
    fs::create_directories(scratch.path() / "hooks");
    std::ofstream hookOut{scratch.path() / "hooks" / "decrypt.js"};
    hookOut << "export default function (ctx) { return ctx.response; }\n";
    hookOut.close();

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookPostResponseFromFile
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  encrypted:
    operations:
      get:
        method: GET
        path: /api/v1/blob
        actor: user
        post_response: hooks/decrypt.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& get = result->resources.at(ce::ResourceId{"encrypted"})
                           .operations.at("get");
    ASSERT_TRUE(get.postResponseScript.has_value());
    EXPECT_NE(get.postResponseScript->find("ctx.response"),
              std::string::npos);
}

TEST(SchemaParserHooks, inline_js_with_braces_falls_through_unchanged) {
    // Anything containing `{`, `(`, `=`, or a newline is treated as
    // inline JS regardless of suffix — protects users who happen to
    // write something like `const x = 1; // foo.js` from being
    // mis-parsed as a path.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookInline
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  one:
    operations:
      send:
        method: POST
        path: /api/v1/x
        actor: user
        pre_request: |
          ctx.request.headers['X-Inline'] = 'yes';
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& send = result->resources.at(ce::ResourceId{"one"})
                            .operations.at("send");
    ASSERT_TRUE(send.preRequestScript.has_value());
    EXPECT_NE(send.preRequestScript->find("X-Inline"),
              std::string::npos);
}

TEST(SchemaParserHooks, rejects_path_traversal_outside_project_root) {
    // Containment check fires on `../../etc/passwd`-style patterns
    // even when the canonicalised target exists. PRD §5.10 +
    // AGENTS.md "Reading user input" §"Path inputs".
    ScratchDir scratch;

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookEscape
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  one:
    operations:
      send:
        method: POST
        path: /api/v1/x
        actor: user
        pre_request: ../../../etc/passwd.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(result.error().detail.find("escapes"),
              std::string::npos);
}

TEST(SchemaParserHooks, rejects_absolute_path) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookAbsolute
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  one:
    operations:
      send:
        method: POST
        path: /api/v1/x
        actor: user
        pre_request: /etc/passwd.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(result.error().detail.find("must be relative"),
              std::string::npos);
}

TEST(SchemaParserHooks, rejects_missing_hook_file) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookMissing
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  one:
    operations:
      send:
        method: POST
        path: /api/v1/x
        actor: user
        pre_request: ./does-not-exist.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(result.error().detail.find("not found"),
              std::string::npos);
}

TEST(SchemaParserHooks, rejects_oversized_hook_file) {
    // 1 MiB cap. Write 1 MiB + 1 of `x` and confirm the parser refuses.
    ScratchDir scratch;
    const fs::path hookPath = scratch.path() / "huge.js";
    {
        std::ofstream out{hookPath, std::ios::binary};
        std::string filler(1024, 'x');
        for (std::size_t i = 0; i < 1024 + 1; ++i) {  // 1 MiB + 1 KiB
            out << filler;
        }
    }

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HookOversized
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { t: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.t}}" } }

resources:
  one:
    operations:
      send:
        method: POST
        path: /api/v1/x
        actor: user
        pre_request: ./huge.js
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(result.error().detail.find("1 MiB"),
              std::string::npos);
}


// ─── Slice 4f — oauth1 auth strategy ────────────────────────────────────────

TEST(SchemaParserOAuth1, parses_three_legged_with_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OAuth1Sample
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  twitter:
    auth:
      strategy: oauth1
      consumer_key: "{{secret.OAUTH_KEY}}"
      consumer_secret: "{{secret.OAUTH_SECRET}}"
      token: "{{secret.OAUTH_TOKEN}}"
      token_secret: "{{secret.OAUTH_TOKEN_SECRET}}"
      realm: "Photos"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: twitter
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& tw = result->actors.at(ce::ActorId{"twitter"});
    EXPECT_EQ(tw.strategy, ce::AuthStrategy::OAuth1);
    EXPECT_TRUE(tw.authSteps.empty());
    EXPECT_EQ(tw.authConfig.at("consumer_key"),    "{{secret.OAUTH_KEY}}");
    EXPECT_EQ(tw.authConfig.at("consumer_secret"), "{{secret.OAUTH_SECRET}}");
    EXPECT_EQ(tw.authConfig.at("token"),           "{{secret.OAUTH_TOKEN}}");
    EXPECT_EQ(tw.authConfig.at("token_secret"),
              "{{secret.OAUTH_TOKEN_SECRET}}");
    EXPECT_EQ(tw.authConfig.at("realm"), "Photos");
}

TEST(SchemaParserOAuth1, parses_two_legged_minimal) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OAuth1TwoLegged
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  app:
    auth:
      strategy: oauth1
      consumer_key: "ck"
      consumer_secret: "cs"

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: app
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& app = result->actors.at(ce::ActorId{"app"});
    EXPECT_EQ(app.strategy, ce::AuthStrategy::OAuth1);
    EXPECT_EQ(app.authConfig.at("consumer_key"),    "ck");
    EXPECT_EQ(app.authConfig.at("consumer_secret"), "cs");
    EXPECT_FALSE(app.authConfig.contains("token"));
    EXPECT_FALSE(app.authConfig.contains("token_secret"));
    EXPECT_FALSE(app.authConfig.contains("realm"));
}

// ─── Slice 4g — aws_sigv4 auth strategy ─────────────────────────────────────

TEST(SchemaParserAwsSigV4, parses_full_options) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: AwsSample
default_environment: local

environment:
  baseUrl: https://iam.amazonaws.com

actors:
  aws:
    auth:
      strategy: aws_sigv4
      access_key: "{{secret.AWS_ACCESS_KEY}}"
      secret_key: "{{secret.AWS_SECRET_KEY}}"
      region: "us-east-1"
      service: "iam"
      session_token: "{{secret.AWS_SESSION_TOKEN}}"
      sign_payload: true

resources:
  list:
    operations:
      run:
        method: GET
        path: /?Action=ListUsers&Version=2010-05-08
        actor: aws
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& aws = result->actors.at(ce::ActorId{"aws"});
    EXPECT_EQ(aws.strategy, ce::AuthStrategy::AwsSigV4);
    EXPECT_TRUE(aws.authSteps.empty());
    EXPECT_EQ(aws.authConfig.at("access_key"),    "{{secret.AWS_ACCESS_KEY}}");
    EXPECT_EQ(aws.authConfig.at("secret_key"),    "{{secret.AWS_SECRET_KEY}}");
    EXPECT_EQ(aws.authConfig.at("region"),        "us-east-1");
    EXPECT_EQ(aws.authConfig.at("service"),       "iam");
    EXPECT_EQ(aws.authConfig.at("session_token"),
              "{{secret.AWS_SESSION_TOKEN}}");
    EXPECT_EQ(aws.authConfig.at("sign_payload"),  "true");
}

TEST(SchemaParserAwsSigV4, optional_fields_are_omitted_when_absent) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: AwsMinimal
default_environment: local

environment:
  baseUrl: https://s3.amazonaws.com

actors:
  s3:
    auth:
      strategy: aws_sigv4
      access_key: "AKIAEXAMPLE"
      secret_key: "secret"
      region: "us-east-1"
      service: "s3"

resources:
  bucket:
    operations:
      list:
        method: GET
        path: /
        actor: s3
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& s3 = result->actors.at(ce::ActorId{"s3"});
    EXPECT_EQ(s3.strategy, ce::AuthStrategy::AwsSigV4);
    EXPECT_FALSE(s3.authConfig.contains("session_token"));
    EXPECT_FALSE(s3.authConfig.contains("sign_payload"));
}
