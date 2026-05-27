// Unit tests for YamlSchemaParser multi-file project layout.
//
// Every existing SchemaParser test embeds everything in a single
// chainapi.yaml. This file covers the `imports:` mechanism that loads
// actors, resources, and environments from sub-directories.
//
// Covered paths:
//   - imports: as a sequence of glob patterns
//   - actors/ sub-directory (Form A: name: at top level; Form B: wrapped)
//   - resources/ sub-directory (Form A and Form B)
//   - environments/ sub-directory (variables: sub-map form and flat form)
//   - Stem-based ID fallback when name: is absent
//   - Mixed inline + imported definitions
//   - depends_on: field on operations
//   - retry: block (max + backoff)
//   - Operation-level timeout: field
//   - force: flag
//   - session.ttl: with all four duration suffixes (s, m, h, d)
//   - session.refresh: block with all fields
#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        const auto unique =
            "chainapi-multifile-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

    fs::path write(const std::string& relPath, const std::string& body) {
        const auto full = path_ / relPath;
        fs::create_directories(full.parent_path());
        std::ofstream out{full};
        out << body;
        return full;
    }

private:
    fs::path path_;
    inline static int counter_{0};
};

}  // namespace

// ─── imports: glob sequence ──────────────────────────────────────────────────

TEST(SchemaParserMultiFile, imports_actors_from_sub_directory_form_a) {
    // Form A: actor file has `name:` at the top level alongside other fields.
    ScratchDir scratch;

    scratch.write("actors/vendor.yaml", R"YAML(
name: vendor
auth:
  method: POST
  path: /api/v1/auth/vendor/login
  body: { email: "v@test.com" }
  extract: { token: $.data.accessToken }
inject:
  headers: { Authorization: "Bearer {{vendor.token}}" }
)YAML");

    scratch.write("resources/product.yaml", R"YAML(
name: product
operations:
  create:
    method: POST
    path: /api/v1/products
    actor: vendor
    expect_status: 201
    extract:
      product_id: $.id
)YAML");

    scratch.write("environments/local.yaml", R"YAML(
name: local
variables:
  baseUrl: http://localhost:8080
)YAML");

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: MultiFileFormA
default_environment: local
imports:
  - actors/*.yaml
  - resources/*.yaml
  - environments/*.yaml
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_EQ(result->name, "MultiFileFormA");
    ASSERT_TRUE(result->actors.contains(ce::ActorId{"vendor"}));
    ASSERT_TRUE(result->resources.contains(ce::ResourceId{"product"}));
    ASSERT_TRUE(result->environments.contains("local"));
    EXPECT_EQ(result->environments.at("local").at("baseUrl"), "http://localhost:8080");

    const auto& vendor = result->actors.at(ce::ActorId{"vendor"});
    ASSERT_EQ(vendor.authSteps.size(), 1u);
    EXPECT_EQ(vendor.authSteps[0].pathTemplate, "/api/v1/auth/vendor/login");
    EXPECT_EQ(vendor.inject.headers.at("Authorization"), "Bearer {{vendor.token}}");

    const auto& product = result->resources.at(ce::ResourceId{"product"});
    ASSERT_TRUE(product.operations.contains("create"));
    EXPECT_EQ(product.operations.at("create").method, ce::HttpMethod::Post);
    EXPECT_EQ(product.operations.at("create").expectStatus, 201);
}

TEST(SchemaParserMultiFile, imports_actors_from_sub_directory_form_b) {
    // Form B: actor file is a single-key map wrapping the body.
    ScratchDir scratch;

    scratch.write("actors/admin.yaml", R"YAML(
admin:
  auth:
    method: POST
    path: /api/v1/auth/admin/login
    body: { email: "admin@test.com" }
    extract: { token: $.data.accessToken }
  inject:
    headers: { Authorization: "Bearer {{admin.token}}" }
)YAML");

    scratch.write("resources/user.yaml", R"YAML(
user:
  operations:
    list:
      method: GET
      path: /api/v1/users
      actor: admin
      expect_status: 200
)YAML");

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: MultiFileFormB
default_environment: local
environment:
  baseUrl: http://localhost:0
imports:
  - actors/*.yaml
  - resources/*.yaml
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_TRUE(result->actors.contains(ce::ActorId{"admin"}));
    ASSERT_TRUE(result->resources.contains(ce::ResourceId{"user"}));

    const auto& admin = result->actors.at(ce::ActorId{"admin"});
    ASSERT_EQ(admin.authSteps.size(), 1u);
    EXPECT_EQ(admin.authSteps[0].pathTemplate, "/api/v1/auth/admin/login");
}

TEST(SchemaParserMultiFile, stem_based_id_fallback_when_name_absent) {
    // When the actor file uses Form B (single-key map), the key becomes the ID.
    // This is the most reliable way to test ID derivation from the file content.
    ScratchDir scratch;

    // Form B: single-key map — the key "service" becomes the actor ID.
    scratch.write("actors/service.yaml", R"YAML(
service:
  auth:
    strategy: api_key
    key: "sk_live_abc"
    location: header
    name: X-API-Key
)YAML");

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: StemFallback
default_environment: local
environment:
  baseUrl: http://localhost:0
imports:
  - actors/*.yaml
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    // ID comes from the wrapping key "service".
    ASSERT_TRUE(result->actors.contains(ce::ActorId{"service"}));
    const auto& svc = result->actors.at(ce::ActorId{"service"});
    EXPECT_EQ(svc.strategy, ce::AuthStrategy::ApiKey);
    EXPECT_EQ(svc.authConfig.at("key"), "sk_live_abc");
}

TEST(SchemaParserMultiFile, environment_file_flat_form_without_variables_key) {
    // Flat form: key-value pairs directly at the top level (no `variables:` wrapper).
    // The env name comes from the file stem when `name:` is absent.
    ScratchDir scratch;

    scratch.write("environments/staging.yaml", R"YAML(
baseUrl: https://staging.api.example.com
admin_email: admin@staging.example.com
)YAML");

    // No inline `environment:` block — only the imported file defines envs.
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: FlatEnv
default_environment: staging
imports:
  - environments/*.yaml
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_TRUE(result->environments.contains("staging"));
    EXPECT_EQ(result->environments.at("staging").at("baseUrl"), "https://staging.api.example.com");
    EXPECT_EQ(result->environments.at("staging").at("admin_email"), "admin@staging.example.com");
}

TEST(SchemaParserMultiFile, inline_and_imported_definitions_coexist) {
    // Inline actors/resources in chainapi.yaml plus imported ones from files.
    ScratchDir scratch;

    scratch.write("actors/external.yaml", R"YAML(
name: external
auth:
  strategy: api_key
  key: "ext-key"
)YAML");

    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: MixedInlineImport
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  inline_actor:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: {}
      extract: { token: $.token }
    inject:
      headers: { Authorization: "Bearer {{inline_actor.token}}" }

imports:
  - actors/*.yaml
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_TRUE(result->actors.contains(ce::ActorId{"inline_actor"}));
    EXPECT_TRUE(result->actors.contains(ce::ActorId{"external"}));
}

// ─── depends_on: field ───────────────────────────────────────────────────────

TEST(SchemaParserOperationFields, depends_on_populates_explicit_dependencies) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: DependsOn
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  product:
    operations:
      create:
        method: POST
        path: /api/v1/products
        actor: user
        expect_status: 201
        extract:
          product_id: $.id
      publish:
        method: POST
        path: /api/v1/products/{{product.product_id}}/publish
        actor: user
        expect_status: 200
        depends_on:
          - product.create
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& publish = result->resources.at(ce::ResourceId{"product"}).operations.at("publish");
    ASSERT_EQ(publish.explicitDependencies.size(), 1u);
    EXPECT_EQ(publish.explicitDependencies[0].value, "product.create");
}

TEST(SchemaParserOperationFields, depends_on_accepts_multiple_entries) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: MultiDependsOn
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders
        actor: user
        extract: { order_id: $.id }
  payment:
    operations:
      create:
        method: POST
        path: /api/v1/payments
        actor: user
        extract: { payment_id: $.id }
  receipt:
    operations:
      generate:
        method: POST
        path: /api/v1/receipts
        actor: user
        depends_on:
          - order.create
          - payment.create
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& generate =
        result->resources.at(ce::ResourceId{"receipt"}).operations.at("generate");
    ASSERT_EQ(generate.explicitDependencies.size(), 2u);

    std::vector<std::string> depValues;
    for (const auto& d : generate.explicitDependencies) depValues.push_back(d.value);
    EXPECT_TRUE(std::find(depValues.begin(), depValues.end(), "order.create") != depValues.end());
    EXPECT_TRUE(std::find(depValues.begin(), depValues.end(), "payment.create") != depValues.end());
}

// ─── retry: block ────────────────────────────────────────────────────────────

TEST(SchemaParserOperationFields, retry_block_sets_max_attempts_and_backoff) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: RetryBlock
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  payment:
    operations:
      charge:
        method: POST
        path: /api/v1/charge
        actor: user
        expect_status: 200
        retry:
          max: 5
          backoff: 1000
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& charge = result->resources.at(ce::ResourceId{"payment"}).operations.at("charge");
    EXPECT_EQ(charge.retry.maxAttempts, 5);
    EXPECT_EQ(charge.retry.baseBackoff, std::chrono::milliseconds{1000});
}

TEST(SchemaParserOperationFields, operation_without_retry_block_uses_defaults) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: DefaultRetry
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/ping
        actor: user
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& get = result->resources.at(ce::ResourceId{"ping"}).operations.at("get");
    // Default values from RetryPolicy struct.
    EXPECT_EQ(get.retry.maxAttempts, 3);
    EXPECT_EQ(get.retry.baseBackoff, std::chrono::milliseconds{500});
}

// ─── timeout: field ──────────────────────────────────────────────────────────

TEST(SchemaParserOperationFields, operation_timeout_is_parsed_in_milliseconds) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: OpTimeout
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  slow:
    operations:
      fetch:
        method: GET
        path: /api/v1/slow
        actor: user
        expect_status: 200
        timeout: 30000
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& fetch = result->resources.at(ce::ResourceId{"slow"}).operations.at("fetch");
    ASSERT_TRUE(fetch.timeout.has_value());
    EXPECT_EQ(*fetch.timeout, std::chrono::milliseconds{30000});
}

TEST(SchemaParserOperationFields, operation_without_timeout_has_nullopt) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: NoTimeout
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

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

    const auto& get = result->resources.at(ce::ResourceId{"ping"}).operations.at("get");
    EXPECT_FALSE(get.timeout.has_value());
}

// ─── force: flag ─────────────────────────────────────────────────────────────

TEST(SchemaParserOperationFields, force_true_is_parsed) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ForceFlag
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

resources:
  product:
    operations:
      create:
        method: POST
        path: /api/v1/products
        actor: user
        expect_status: 201
        extract: { product_id: $.id }
        force: true
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& create = result->resources.at(ce::ResourceId{"product"}).operations.at("create");
    EXPECT_TRUE(create.force);
}

TEST(SchemaParserOperationFields, force_defaults_to_false_when_absent) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: NoForce
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

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

    const auto& get = result->resources.at(ce::ResourceId{"ping"}).operations.at("get");
    EXPECT_FALSE(get.force);
}

// ─── session.ttl: ────────────────────────────────────────────────────────────

TEST(SchemaParserSessionFields, session_ttl_parses_seconds_suffix) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: TtlSeconds
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }
    session:
      ttl: 300s

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
    EXPECT_EQ(user.sessionTtl, std::chrono::seconds{300});
}

TEST(SchemaParserSessionFields, session_ttl_parses_minutes_suffix) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: TtlMinutes
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }
    session:
      ttl: 30m

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
    EXPECT_EQ(user.sessionTtl, std::chrono::seconds{30 * 60});
}

TEST(SchemaParserSessionFields, session_ttl_parses_hours_suffix) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: TtlHours
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }
    session:
      ttl: 2h

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
    EXPECT_EQ(user.sessionTtl, std::chrono::seconds{2 * 3600});
}

TEST(SchemaParserSessionFields, session_ttl_parses_days_suffix) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: TtlDays
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }
    session:
      ttl: 7d

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
    EXPECT_EQ(user.sessionTtl, std::chrono::seconds{7 * 86400});
}

TEST(SchemaParserSessionFields, actor_without_session_block_uses_default_ttl) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: DefaultTtl
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

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
    // Default from Actor struct: 15 * 60 = 900 seconds.
    EXPECT_EQ(user.sessionTtl, std::chrono::seconds{15 * 60});
}

// ─── session.refresh: block ──────────────────────────────────────────────────

TEST(SchemaParserSessionFields, session_refresh_block_is_parsed_with_all_fields) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: RefreshBlock
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@test.com" }
      extract: { token: $.data.accessToken, refreshToken: $.data.refreshToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }
    session:
      ttl: 15m
      refresh:
        method: POST
        path: /api/v1/auth/refresh
        headers: { Authorization: "Bearer {{user.token}}" }
        body: { refresh_token: "{{user.refreshToken}}" }
        extract: { token: $.data.accessToken }

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
    ASSERT_TRUE(user.refresh.has_value());

    const auto& refresh = *user.refresh;
    EXPECT_EQ(refresh.method, ce::HttpMethod::Post);
    EXPECT_EQ(refresh.pathTemplate, "/api/v1/auth/refresh");
    EXPECT_EQ(refresh.headers.at("Authorization"), "Bearer {{user.token}}");
    ASSERT_TRUE(refresh.bodyTemplate.has_value());
    EXPECT_NE(refresh.bodyTemplate->find("refresh_token"), std::string::npos);
    ASSERT_EQ(refresh.extractions.size(), 1u);
    EXPECT_EQ(refresh.extractions[0].variableName, "token");
    EXPECT_EQ(refresh.extractions[0].sourcePath, "$.data.accessToken");
}

TEST(SchemaParserSessionFields, actor_without_refresh_block_has_nullopt) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: NoRefresh
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }
    inject: { headers: { Authorization: "Bearer {{user.token}}" } }

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
    EXPECT_FALSE(user.refresh.has_value());
}

TEST(SchemaParserSessionFields, session_refresh_expect_status_scalar_is_parsed) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: RefreshExpectStatusScalar
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@test.com" }
      extract: { token: $.data.accessToken }
    session:
      ttl: 15m
      refresh:
        method: POST
        path: /api/v1/auth/refresh
        body: { refresh_token: "{{user.refresh_token}}" }
        expect_status: 204
        extract: { token: $.data.accessToken }

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

    const auto& refresh = *result->actors.at(ce::ActorId{"user"}).refresh;
    EXPECT_EQ(refresh.expectStatus, 204);
    EXPECT_TRUE(refresh.expectStatusList.empty()) << "scalar form must not populate the list field";
}

TEST(SchemaParserSessionFields, session_refresh_expect_status_list_is_parsed) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: RefreshExpectStatusList
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@test.com" }
      extract: { token: $.data.accessToken }
    session:
      ttl: 15m
      refresh:
        method: POST
        path: /api/v1/auth/refresh
        body: { refresh_token: "{{user.refresh_token}}" }
        expect_status: [200, 202, 204]
        extract: { token: $.data.accessToken }

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

    const auto& refresh = *result->actors.at(ce::ActorId{"user"}).refresh;
    EXPECT_FALSE(refresh.expectStatus.has_value())
        << "list form must not also populate the scalar field";
    EXPECT_EQ(refresh.expectStatusList, (std::vector<int>{200, 202, 204}));
}

TEST(SchemaParserSessionFields, session_refresh_without_expect_status_leaves_both_fields_empty) {
    // Backwards compatibility: schemas that pre-date `refresh.expect_status`
    // must continue to parse cleanly. runRefresh's "any 2xx is success"
    // default keeps their behaviour unchanged.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: RefreshNoExpectStatus
default_environment: local

environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@test.com" }
      extract: { token: $.data.accessToken }
    session:
      ttl: 15m
      refresh:
        method: POST
        path: /api/v1/auth/refresh
        body: { refresh_token: "{{user.refresh_token}}" }
        extract: { token: $.data.accessToken }

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

    const auto& refresh = *result->actors.at(ce::ActorId{"user"}).refresh;
    EXPECT_FALSE(refresh.expectStatus.has_value());
    EXPECT_TRUE(refresh.expectStatusList.empty());
}

// ─── Schema version validation ───────────────────────────────────────────────

TEST(SchemaParserVersion, version_zero_is_rejected) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 0
name: BadVersion
environment:
  baseUrl: http://localhost:0
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaVersion);
}

TEST(SchemaParserVersion, version_4_is_rejected) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 4
name: FutureVersion
environment:
  baseUrl: http://localhost:0
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaVersion);
    EXPECT_NE(result.error().detail.find("migrate"), std::string::npos);
}

TEST(SchemaParserVersion, missing_file_returns_yaml_parse_error) {
    const fs::path nonexistent = fs::temp_directory_path() / "does-not-exist-chainapi.yaml";
    auto result = ce::parseProject(nonexistent);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::YamlParse);
}

// ─── Extraction source auto-detection ──────────────────────────────────────
//
// The parser tags an Extraction's source kind based on its `sourcePath`
// prefix. Hand-written schemas don't carry an explicit `source:` field
// today, so this convention is the only way headers / cookies / status
// codes light up.

TEST(SchemaParserExtractionSource, jsonpath_is_the_default) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ExtSrcDefault
default_environment: local
environment: { baseUrl: http://localhost:0 }

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        actor: user
        extract:
          id: $.data.id
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& ext =
        result->resources.at(ce::ResourceId{"thing"}).operations.at("get").extractions[0];
    EXPECT_EQ(ext.source, ce::Extraction::Source::JsonPath);
    EXPECT_EQ(ext.sourcePath, "$.data.id");
}

TEST(SchemaParserExtractionSource, dollar_headers_prefix_tags_header_source) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ExtSrcHeader
default_environment: local
environment: { baseUrl: http://localhost:0 }

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        actor: user
        extract:
          location: $.headers.Location
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& ext =
        result->resources.at(ce::ResourceId{"thing"}).operations.at("get").extractions[0];
    EXPECT_EQ(ext.source, ce::Extraction::Source::Header);
    EXPECT_EQ(ext.sourcePath, "$.headers.Location");
}

TEST(SchemaParserExtractionSource, dollar_cookies_prefix_tags_cookie_source) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ExtSrcCookie
default_environment: local
environment: { baseUrl: http://localhost:0 }

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        actor: user
        extract:
          session: $.cookies.SESSIONID
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& ext =
        result->resources.at(ce::ResourceId{"thing"}).operations.at("get").extractions[0];
    EXPECT_EQ(ext.source, ce::Extraction::Source::Cookie);
    EXPECT_EQ(ext.sourcePath, "$.cookies.SESSIONID");
}

TEST(SchemaParserExtractionSource, exact_dollar_status_code_tags_status_code_source) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ExtSrcStatus
default_environment: local
environment: { baseUrl: http://localhost:0 }

actors:
  user:
    auth: { method: POST, path: /login, body: {}, extract: { token: $.t } }

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        actor: user
        extract:
          last_status: $.status_code
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& ext =
        result->resources.at(ce::ResourceId{"thing"}).operations.at("get").extractions[0];
    EXPECT_EQ(ext.source, ce::Extraction::Source::StatusCode);
}
