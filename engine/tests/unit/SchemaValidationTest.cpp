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
        const auto unique = "chainapi-schema-valid-" + std::to_string(::getpid()) + "-" +
                            std::to_string(counter_++);
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

TEST(SchemaValidation, undefined_reference_with_surrounding_whitespace_fails_load) {
    // The runtime resolver matches {{[^}]+}} then trims, so a reference
    // with inner whitespace resolves at run time. Parse-time validation
    // must use the SAME grammar — otherwise `{{ ghost.id }}` (spaces)
    // slips past the undefined-ref check. Regression for the scanner
    // grammar that previously required `{{word.word}}` with no spaces.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: WhitespaceGhost
environment:
  baseUrl: http://localhost:0

resources:
  order:
    operations:
      create:
        method: POST
        path: "/api/v1/orders/{{ ghost.id }}"
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("ghost"), std::string::npos);
}

TEST(SchemaValidation, implicit_cycle_with_whitespace_references_fails_load) {
    // Same grammar concern, on the cycle path: whitespaced implicit
    // edges must still be inferred so the cycle is detected at load.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: WhitespaceCycle
environment:
  baseUrl: http://localhost:0

resources:
  a:
    operations:
      op:
        method: GET
        path: "/a/{{ b.b_id }}"
        expect_status: 200
        extract:
          a_id: $.id
  b:
    operations:
      op:
        method: GET
        path: "/b/{{ a.a_id }}"
        expect_status: 200
        extract:
          b_id: $.id
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

TEST(SchemaValidation, builtin_call_embedding_a_secret_reference_loads) {
    // {{$.base64.encode(secret.API_KEY)}} — the scanner sees scope "$"
    // (a builtin); the nested secret.API_KEY resolves inside the
    // builtin at run time and must NOT be flagged undefined at load.
    ScratchDir scratch;
    const auto yaml = scratch.write("chainapi.yaml", R"YAML(
version: 1
name: BuiltinCallOk
environment:
  baseUrl: http://localhost:0

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/v1/thing
        headers:
          Authorization: "Basic {{$.base64.encode(secret.API_KEY)}}"
        expect_status: 200
)YAML");

    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
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

// ─── Resource-exhaustion guards (untrusted YAML is an attacker surface) ──────
//
// AGENTS.md §"Reading user input" names chainapi.yaml as attacker-controlled
// and requires depth + document-size caps before parsing. These tests feed
// the parser hostile input and assert it fails cleanly with a YamlParse error
// rather than exhausting memory or overflowing the stack.

TEST(SchemaValidation, oversized_schema_document_is_rejected) {
    // A document past the 8 MiB cap must be refused before yaml-cpp sees it,
    // so a multi-gigabyte file can't be slurped into memory. We pad a comment
    // block so the bytes are valid YAML — the size check, not a parse error,
    // is what must trip.
    ScratchDir scratch;
    std::string yaml = "version: 1\nname: Huge\nenvironment:\n  baseUrl: http://localhost:0\n";
    yaml.reserve(yaml.size() + (9u * 1024 * 1024) + 16);
    yaml.append("# ");
    yaml.append(static_cast<std::size_t>(9) * 1024 * 1024, 'x');
    yaml.push_back('\n');
    const auto path = scratch.write("chainapi.yaml", yaml);

    auto result = ce::parseProject(path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::YamlParse);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Schema);
}

TEST(SchemaValidation, deeply_nested_yaml_is_rejected_not_crashing) {
    // Flow-style nested sequences. yaml-cpp's DepthGuard (default 2000) trips
    // during LoadFile and throws DeepRecursion (a YAML::Exception subclass),
    // which parse() maps to a YamlParse error instead of overflowing the
    // parser's recursion stack.
    ScratchDir scratch;
    constexpr int kDepth = 5000;
    std::string nested(static_cast<std::size_t>(kDepth), '[');
    nested.append(static_cast<std::size_t>(kDepth), ']');
    const std::string yaml =
        "version: 1\nname: DeepNest\nenvironment:\n  baseUrl: http://localhost:0\ndeep: " + nested +
        "\n";
    const auto path = scratch.write("chainapi.yaml", yaml);

    auto result = ce::parseProject(path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::YamlParse);
}

TEST(SchemaValidation, deeply_nested_operation_body_is_rejected_not_crashing) {
    // The body→JSON conversion (yamlNodeToJsonValue) recurses independently of
    // yaml-cpp. Its kMaxYamlDepth cap must reject a body nested past the limit
    // with a YamlParse error rather than a stack overflow. 200 levels clears
    // the 64 cap without tripping yaml-cpp's own 2000 structural guard first.
    ScratchDir scratch;
    constexpr int kDepth = 200;
    std::string body(static_cast<std::size_t>(kDepth), '[');
    body.append(static_cast<std::size_t>(kDepth), ']');
    const std::string yaml = R"YAML(
version: 1
name: DeepBody
environment:
  baseUrl: http://localhost:0

resources:
  thing:
    operations:
      create:
        method: POST
        path: /api/v1/thing
        expect_status: 200
        body: )YAML" + body + "\n";
    const auto path = scratch.write("chainapi.yaml", yaml);

    auto result = ce::parseProject(path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::YamlParse);
}
