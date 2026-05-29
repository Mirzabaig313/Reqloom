// SchemaValidationTest — parse-time enforcement of the load-time
//
//   cyclic dependency graph → project refuses to load (E_CYCLE)
//   self-dependency is a cycle
//    reference to an undefined symbol → refuses to load
//             (E_REF_UNDEFINED)
//
// These previously only surfaced when DependencyResolver::resolve() ran
// at execution time, so a cyclic project loaded cleanly and only blew
// up when the user clicked Run. Each test below fails on the parent
// commit because parse() returned the malformed Project unchallenged.

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

class ScratchDir {
public:
    ScratchDir() {
        const auto unique =
            "chainapi-schema-valid-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

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

// ───— undefined reference ──────────────────────────────────────────

TEST(SchemaValidation, undefined_resource_reference_fails_load) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: GhostRef
environment:
  baseUrl: http://localhost:0

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders/{{ghost.id}}
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Schema);
    EXPECT_NE(result.error().detail.find("ghost"), std::string::npos);
}

TEST(SchemaValidation, env_and_secret_references_are_accepted) {
    // env / secret roots resolve at run time against the environment
    // and secret store — they must NOT be flagged as undefined.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: EnvSecretOk
environment:
  baseUrl: http://localhost:0

resources:
  thing:
    operations:
      get:
        method: GET
        path: "{{env.baseUrl}}/api/v1/thing"
        headers:
          X-Api-Key: "{{secret.API_KEY}}"
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

TEST(SchemaValidation, builtin_dollar_references_are_accepted) {
    // $.uuid / $.now and friends are builtins, not symbols — accepted.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: BuiltinOk
environment:
  baseUrl: http://localhost:0

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders
        body: { idempotency_key: "{{$.uuid}}", at: "{{$.now}}" }
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

TEST(SchemaValidation, actor_reference_is_accepted) {
    // {{user.token}} resolves against the actor session, not a resource.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ActorOk
environment:
  baseUrl: http://localhost:0

actors:
  user:
    auth:
      method: POST
      path: /login
      body: { email: "a@b.test" }
      extract: { token: $.token }

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        headers:
          Authorization: "Bearer {{user.token}}"
        actor: user
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

TEST(SchemaValidation, undefined_depends_on_target_fails_load) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: GhostDep
environment:
  baseUrl: http://localhost:0

resources:
  order:
    operations:
      pay:
        method: POST
        path: /api/v1/pay
        depends_on: [order.nonexistent]
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("order.nonexistent"), std::string::npos);
}

// ───— cycle detection ──────────────────────────────────────────────

TEST(SchemaValidation, explicit_two_node_cycle_fails_load) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: TwoNodeCycle
environment:
  baseUrl: http://localhost:0

resources:
  a:
    operations:
      op:
        method: GET
        path: /a
        depends_on: [b.op]
        expect_status: 200
  b:
    operations:
      op:
        method: GET
        path: /b
        depends_on: [a.op]
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Schema);
    // Both operations should be named in the cycle report.
    EXPECT_NE(result.error().detail.find("a.op"), std::string::npos);
    EXPECT_NE(result.error().detail.find("b.op"), std::string::npos);
}

TEST(SchemaValidation, implicit_cycle_via_templates_fails_load) {
    // a.op extracts a_id and references {{b.b_id}}; b.op extracts b_id
    // and references {{a.a_id}}. Implicit edges form a cycle.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: ImplicitCycle
environment:
  baseUrl: http://localhost:0

resources:
  a:
    operations:
      op:
        method: GET
        path: /a/{{b.b_id}}
        expect_status: 200
        extract:
          a_id: $.id
  b:
    operations:
      op:
        method: GET
        path: /b/{{a.a_id}}
        expect_status: 200
        extract:
          b_id: $.id
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

// ───— self-dependency is a cycle ───────────────────────────────────

TEST(SchemaValidation, self_dependency_via_explicit_depends_on_fails_load) {
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: SelfDep
environment:
  baseUrl: http://localhost:0

resources:
  loop:
    operations:
      op:
        method: GET
        path: /loop
        depends_on: [loop.op]
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
    EXPECT_NE(result.error().detail.find("loop.op"), std::string::npos);
}

// ─── Healthy multi-op project still loads ────────────────────────────────────

TEST(SchemaValidation, acyclic_multi_resource_project_loads_cleanly) {
    // A small but realistic chain: order.create → order.pay (implicit
    // via {{order.order_id}}). No cycle, all references defined.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: HealthyChain
environment:
  baseUrl: http://localhost:0

resources:
  order:
    operations:
      create:
        method: POST
        path: /api/v1/orders
        expect_status: 200
        extract:
          order_id: $.id
      pay:
        method: POST
        path: /api/v1/orders/{{order.order_id}}/pay
        depends_on: [order.create]
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->resources.size(), 1u);
    EXPECT_EQ(result->resources.at(ce::ResourceId{"order"}).operations.size(), 2u);
}
