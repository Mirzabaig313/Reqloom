// Load-time strictness of YamlSchemaParser: wrong types, unknown enum values, malformed
// durations and duplicate ids fail the load naming the file; unknown keys and empty imports
// are warnings; filesystem errors, alias expansion and error locations are bounded.
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = reqloom::tests::uniqueTempPath("reqloom-strict");
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(path_, ec)) {
            fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

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
};

constexpr const char* kImportingRoot = R"YAML(version: 1
name: Strict
default_environment: local
imports:
  - actors/*.yaml
  - resources/*.yaml
  - environments/*.yaml
)YAML";

std::string rootWithResource(const std::string& opBody) {
    return std::string{
               "version: 1\nname: Strict\nresources:\n  users:\n    operations:\n"
               "      create:\n        method: POST\n        path: /users\n"} +
           opBody;
}

std::string loadError(ScratchDir& scratch, const std::string& rootYaml) {
    const auto yaml = scratch.write("reqloom.yaml", rootYaml);
    auto result = ce::parseProject(yaml);
    if (result) {
        return "<loaded>";
    }
    return result.error().detail;
}

bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::ranges::any_of(
        lines, [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

}  // namespace

// ─── Item 1: silent fallbacks become errors or warnings ─────────────────────

TEST(SchemaStrictness, timeout_with_a_unit_is_an_error_naming_file_key_and_value) {
    ScratchDir scratch;
    const auto err = loadError(scratch, rootWithResource("        timeout: 5s\n"));
    EXPECT_NE(err.find("reqloom.yaml:"), std::string::npos) << err;
    EXPECT_NE(err.find("'timeout'"), std::string::npos) << err;
    EXPECT_NE(err.find("'5s'"), std::string::npos) << err;
}

TEST(SchemaStrictness, retry_backoff_with_a_unit_is_an_error) {
    ScratchDir scratch;
    const auto err =
        loadError(scratch, rootWithResource("        retry: { max: 2, backoff: 1s }\n"));
    EXPECT_NE(err.find("'backoff'"), std::string::npos) << err;
    EXPECT_NE(err.find("'1s'"), std::string::npos) << err;
}

TEST(SchemaStrictness, scalar_depends_on_is_read_as_a_one_item_list) {
    ScratchDir scratch;
    const auto yaml = scratch.write("reqloom.yaml", R"YAML(version: 1
name: Strict
resources:
  users:
    operations:
      list: { method: GET, path: /users }
      create:
        method: POST
        path: /users
        depends_on: users.list
)YAML");
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& deps =
        result->resources.at(ce::ResourceId{"users"}).operations.at("create").explicitDependencies;
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].value, "users.list");
}

TEST(SchemaStrictness, depends_on_map_is_an_error) {
    ScratchDir scratch;
    const auto err = loadError(scratch, rootWithResource("        depends_on: { a: b }\n"));
    EXPECT_NE(err.find("'depends_on'"), std::string::npos) << err;
}

TEST(SchemaStrictness, unknown_method_is_an_error_listing_accepted_values) {
    ScratchDir scratch;
    const auto yaml =
        scratch.write("reqloom.yaml",
                      "version: 1\nname: S\nresources:\n  users:\n    operations:\n      del:\n"
                      "        method: DELTE\n        path: /users\n");
    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    const auto& err = result.error().detail;
    EXPECT_NE(err.find("'DELTE'"), std::string::npos) << err;
    EXPECT_NE(err.find("GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS"), std::string::npos) << err;
    EXPECT_NE(err.find("reqloom.yaml:7:"), std::string::npos) << err;
}

TEST(SchemaStrictness, method_is_case_insensitive) {
    ScratchDir scratch;
    const auto yaml =
        scratch.write("reqloom.yaml",
                      "version: 1\nname: S\nresources:\n  users:\n    operations:\n      create:\n"
                      "        method: Post\n        path: /users\n");
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->resources.at(ce::ResourceId{"users"}).operations.at("create").method,
              ce::HttpMethod::Post);
}

TEST(SchemaStrictness, inline_auth_type_is_case_insensitive) {
    ScratchDir scratch;
    const auto yaml = scratch.write(
        "reqloom.yaml", rootWithResource("        auth: { type: Bearer, token: abc }\n"));
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& auth =
        result->resources.at(ce::ResourceId{"users"}).operations.at("create").inlineAuth;
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->type, ce::InlineAuthType::Bearer);
    EXPECT_EQ(auth->token, "abc");
}

TEST(SchemaStrictness, unknown_inline_auth_type_is_an_error) {
    ScratchDir scratch;
    const auto err =
        loadError(scratch, rootWithResource("        auth: { type: beerer, token: x }\n"));
    EXPECT_NE(err.find("'beerer'"), std::string::npos) << err;
    EXPECT_NE(err.find("bearer"), std::string::npos) << err;
}

TEST(SchemaStrictness, unknown_actor_strategy_is_an_error) {
    ScratchDir scratch;
    scratch.write("actors/svc.yaml", "name: svc\nauth:\n  strategy: oauth3\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("svc.yaml:3:"), std::string::npos) << err;
    EXPECT_NE(err.find("'oauth3'"), std::string::npos) << err;
    EXPECT_NE(err.find("oauth2_client_credentials"), std::string::npos) << err;
}

TEST(SchemaStrictness, unknown_extraction_source_is_an_error) {
    ScratchDir scratch;
    const auto err = loadError(
        scratch, rootWithResource("        extract: { id: { path: $.id, source: jsonpth } }\n"));
    EXPECT_NE(err.find("'jsonpth'"), std::string::npos) << err;
}

TEST(SchemaStrictness, unknown_duration_unit_is_an_error) {
    ScratchDir scratch;
    scratch.write("actors/svc.yaml",
                  "name: svc\nauth: { strategy: bearer, token: t }\nsession: { ttl: 5x }\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("'5x'"), std::string::npos) << err;
    EXPECT_NE(err.find("svc.yaml"), std::string::npos) << err;
}

TEST(SchemaStrictness, malformed_duration_is_an_error) {
    ScratchDir scratch;
    const auto err =
        loadError(scratch,
                  rootWithResource(
                      "        poll_until: { path: /x, success_when: 'true', interval: soon }\n"));
    EXPECT_NE(err.find("'soon'"), std::string::npos) << err;
}

TEST(SchemaStrictness, string_field_given_a_map_is_an_error) {
    ScratchDir scratch;
    const auto yaml =
        scratch.write("reqloom.yaml",
                      "version: 1\nname: S\nresources:\n  users:\n    operations:\n      create:\n"
                      "        method: POST\n        path: { a: 1 }\n");
    auto result = ce::parseProject(yaml);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().detail.find("'path'"), std::string::npos) << result.error().detail;
}

TEST(SchemaStrictness, duplicate_actor_across_files_is_an_error_naming_both) {
    ScratchDir scratch;
    scratch.write("actors/a.yaml", "name: svc\nauth: { strategy: bearer, token: a }\n");
    scratch.write("actors/b.yaml", "name: svc\nauth: { strategy: bearer, token: b }\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("a.yaml"), std::string::npos) << err;
    EXPECT_NE(err.find("b.yaml"), std::string::npos) << err;
    EXPECT_NE(err.find("'svc'"), std::string::npos) << err;
}

TEST(SchemaStrictness, duplicate_resource_between_file_and_root_is_an_error) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list: { method: GET, path: /u }\n");
    const auto err = loadError(scratch,
                               std::string{kImportingRoot} +
                                   "resources:\n  users:\n    operations:\n"
                                   "      get: { method: GET, path: /u/1 }\n");
    EXPECT_NE(err.find("'users'"), std::string::npos) << err;
    EXPECT_NE(err.find("users.yaml"), std::string::npos) << err;
}

TEST(SchemaStrictness, unknown_keys_are_warnings_and_do_not_fail_the_load) {
    ScratchDir scratch;
    const auto yaml =
        scratch.write("reqloom.yaml", rootWithResource("        methd: GET\n") + "colour: blue\n");
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(anyContains(result->loadWarnings, "'methd'"));
    EXPECT_TRUE(anyContains(result->loadWarnings, "'colour'"));
    EXPECT_TRUE(anyContains(result->loadWarnings, "reqloom.yaml:"));
}

TEST(SchemaStrictness, import_pattern_matching_nothing_is_a_warning) {
    ScratchDir scratch;
    const auto yaml = scratch.write(
        "reqloom.yaml", "version: 1\nname: S\nimports:\n  - resourses/*.yaml\n  - ../*.yaml\n");
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(anyContains(result->loadWarnings, "resourses/*.yaml"));
}

TEST(SchemaStrictness, imported_file_outside_managed_folders_is_a_warning) {
    ScratchDir scratch;
    scratch.write("other/x.yaml", "name: x\n");
    const auto yaml =
        scratch.write("reqloom.yaml", "version: 1\nname: S\nimports:\n  - other/*.yaml\n");
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(anyContains(result->loadWarnings, "x.yaml"));
}

// ─── Item 2: flat file with a single known key ───────────────────────────────

TEST(SchemaStrictness, flat_resource_with_only_operations_keeps_the_file_stem_as_id) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml", "operations:\n  list: { method: GET, path: /users }\n");
    const auto yaml = scratch.write("reqloom.yaml", kImportingRoot);
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->resources.contains(ce::ResourceId{"users"}));
    EXPECT_FALSE(result->resources.contains(ce::ResourceId{"operations"}));
    EXPECT_TRUE(result->resources.at(ce::ResourceId{"users"}).operations.contains("list"));
}

TEST(SchemaStrictness, flat_actor_with_only_auth_keeps_the_file_stem_as_id) {
    ScratchDir scratch;
    scratch.write("actors/svc.yaml", "auth: { strategy: bearer, token: t }\n");
    const auto yaml = scratch.write("reqloom.yaml", kImportingRoot);
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->actors.contains(ce::ActorId{"svc"}));
    EXPECT_EQ(result->actors.at(ce::ActorId{"svc"}).strategy, ce::AuthStrategy::Bearer);
}

// ─── Item 3: alias expansion is bounded ──────────────────────────────────────

TEST(SchemaStrictness, alias_expansion_in_a_body_is_capped) {
    ScratchDir scratch;
    std::string body = "        body:\n          l0: &a0 [x, x, x, x, x, x, x, x, x, x]\n";
    for (int level = 1; level <= 8; ++level) {
        body += "          l" + std::to_string(level) + ": &a" + std::to_string(level) + " [";
        for (int i = 0; i < 10; ++i) {
            body += (i == 0 ? "*a" : ", *a") + std::to_string(level - 1);
        }
        body += "]\n";
    }
    const auto err = loadError(scratch, rootWithResource(body));
    EXPECT_NE(err.find("1000000"), std::string::npos) << err;
}

// ─── Item 4: filesystem errors are values, not exceptions ────────────────────

TEST(SchemaStrictness, unreadable_import_directory_returns_an_error_naming_it) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml", "operations:\n  list: { method: GET, path: /u }\n");
    const auto yaml = scratch.write("reqloom.yaml", kImportingRoot);
    fs::permissions(scratch.path() / "resources", fs::perms::none);
    std::error_code probe;
    if (fs::directory_iterator(scratch.path() / "resources", probe); !probe) {
        GTEST_SKIP() << "running with permissions that bypass chmod";
    }
    bool threw = false;
    bool loaded = false;
    std::string detail;
    try {
        auto result = ce::parseProject(yaml);
        loaded = result.has_value();
        if (!loaded) {
            detail = result.error().detail;
        }
    } catch (...) {
        threw = true;
    }
    ASSERT_FALSE(threw);
    ASSERT_FALSE(loaded);
    EXPECT_NE(detail.find("resources"), std::string::npos) << detail;
}

// ─── Item 5: error locations ─────────────────────────────────────────────────

TEST(SchemaStrictness, conversion_error_in_a_sub_file_names_that_file) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list:\n    method: GET\n    path: /u\n"
                  "    expect_status: abc\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("users.yaml:6:"), std::string::npos) << err;
}

TEST(SchemaStrictness, syntax_error_in_a_sub_file_names_that_file_and_line) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml", "name: users\noperations:\n  list: [unclosed\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("users.yaml:"), std::string::npos) << err;
}

TEST(SchemaStrictness, hook_error_names_file_and_operation) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list:\n    method: GET\n    path: /u\n"
                  "    pre_request: ./hooks/missing.js\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("users.yaml:6:"), std::string::npos) << err;
    EXPECT_NE(err.find("users.list"), std::string::npos) << err;
}

TEST(SchemaStrictness, parser_records_where_each_entity_was_defined) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list: { method: GET, path: /u }\n");
    scratch.write("actors/svc.yaml", "name: svc\nauth: { strategy: bearer, token: t }\n");
    scratch.write("environments/local.yaml", "name: local\nvariables: { a: b }\n");
    const auto yaml = scratch.write("reqloom.yaml", kImportingRoot);
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& op = result->resources.at(ce::ResourceId{"users"}).operations.at("list");
    EXPECT_TRUE(op.source.file.ends_with("users.yaml")) << op.source.file;
    EXPECT_EQ(op.source.line, 3);
    EXPECT_TRUE(result->resources.at(ce::ResourceId{"users"}).source.file.ends_with("users.yaml"));
    EXPECT_TRUE(result->actors.at(ce::ActorId{"svc"}).source.file.ends_with("svc.yaml"));
    EXPECT_TRUE(result->environmentSources.at("local").file.ends_with("local.yaml"));
}

TEST(SchemaStrictness, validation_error_carries_the_operation_file_and_line) {
    ScratchDir scratch;
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list:\n    method: GET\n    path: /u\n"
                  "    actor: ghost\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("users.yaml:3"), std::string::npos) << err;
    EXPECT_NE(err.find("ghost"), std::string::npos) << err;
}

// ─── Item 6: id characters checked at load ───────────────────────────────────

TEST(SchemaStrictness, unsafe_actor_id_is_rejected_at_load_naming_file_and_id) {
    ScratchDir scratch;
    scratch.write("actors/svc.yaml", "name: \"a:b\"\nauth: { strategy: bearer, token: t }\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("svc.yaml"), std::string::npos) << err;
    EXPECT_NE(err.find("'a:b'"), std::string::npos) << err;
}

TEST(SchemaStrictness, unsafe_environment_name_is_rejected_by_validate) {
    ce::Project project;
    project.environments["../evil"] = {};
    auto valid = ce::validateProject(project);
    ASSERT_FALSE(valid.has_value());
    EXPECT_NE(valid.error().detail.find("'../evil'"), std::string::npos) << valid.error().detail;
}

// ─── Item 7: max_response_size ───────────────────────────────────────────────

TEST(SchemaStrictness, max_response_size_accepts_bytes_and_iec_units) {
    ScratchDir scratch;
    scratch.write("environments/local.yaml",
                  "name: local\ntransport: { max_response_size: 16MiB }\n");
    scratch.write("environments/ci.yaml", "name: ci\ntransport: { max_response_size: 4096 }\n");
    const auto yaml = scratch.write("reqloom.yaml", kImportingRoot);
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->transport.at("local").maxResponseBytes, std::size_t{16} * 1024 * 1024);
    EXPECT_EQ(result->transport.at("ci").maxResponseBytes, std::size_t{4096});
}

TEST(SchemaStrictness, max_response_size_rejects_other_units) {
    for (const char* bad : {"16MB", "16M", "0", "-1", "1.5MiB", "MiB"}) {
        ScratchDir scratch;
        scratch.write("environments/local.yaml",
                      std::string{"name: local\ntransport: { max_response_size: "} + bad + " }\n");
        const auto err = loadError(scratch, kImportingRoot);
        EXPECT_NE(err.find("max_response_size"), std::string::npos) << bad << ": " << err;
    }
}

TEST(SchemaStrictness, max_response_size_round_trips_and_is_omitted_when_default) {
    ScratchDir scratch;
    ce::Project project;
    project.name = "RT";
    project.defaultEnvironment = "local";
    project.environments["local"] = {};
    project.environments["ci"] = {};
    project.transport["local"].maxResponseBytes = std::size_t{3} * 1024 * 1024;
    project.transport["ci"] = ce::TransportConfig{};
    ASSERT_TRUE(ce::writeProject(scratch.path(), project, /*overwrite=*/true).has_value());

    std::ifstream ci{scratch.path() / "environments" / "ci.yaml"};
    const std::string ciText{std::istreambuf_iterator<char>(ci), std::istreambuf_iterator<char>()};
    EXPECT_EQ(ciText.find("max_response_size"), std::string::npos) << ciText;

    auto reparsed = ce::parseProject(scratch.path() / "reqloom.yaml");
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error().detail;
    EXPECT_EQ(reparsed->transport.at("local").maxResponseBytes, std::size_t{3} * 1024 * 1024);
}

// ─── Item 8: $.env checked in environment values and actor templates ─────────

TEST(SchemaStrictness, env_builtin_in_environment_value_is_checked_at_load) {
    ScratchDir scratch;
    scratch.write("environments/local.yaml",
                  "name: local\nvariables: { home: '{{$.env.HOME}}' }\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("$.env.HOME"), std::string::npos) << err;
    EXPECT_NE(err.find("allowed_env"), std::string::npos) << err;
}

TEST(SchemaStrictness, env_builtin_in_actor_template_is_checked_at_load) {
    ScratchDir scratch;
    scratch.write("actors/svc.yaml",
                  "name: svc\nauth: { strategy: bearer, token: '{{$.env.TOKEN}}' }\n");
    const auto err = loadError(scratch, kImportingRoot);
    EXPECT_NE(err.find("$.env.TOKEN"), std::string::npos) << err;
    EXPECT_NE(err.find("svc"), std::string::npos) << err;
}

TEST(SchemaStrictness, allowed_env_builtin_in_environment_value_loads) {
    ScratchDir scratch;
    scratch.write("environments/local.yaml",
                  "name: local\nvariables: { home: '{{$.env.HOME}}' }\n");
    const auto yaml =
        scratch.write("reqloom.yaml", std::string{kImportingRoot} + "allowed_env: [HOME]\n");
    auto result = ce::parseProject(yaml);
    EXPECT_TRUE(result.has_value()) << result.error().detail;
}

// ─── Item 9: placeholders inside a structured body ───────────────────────────

TEST(SchemaStrictness, placeholder_with_quoted_argument_survives_a_map_body) {
    ScratchDir scratch;
    const auto yaml = scratch.write(
        "reqloom.yaml",
        rootWithResource("        body: { a: '{{$.base64.encode(\"x\")}}', b: 'say \"hi\"' }\n"));
    auto result = ce::parseProject(yaml);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& body =
        result->resources.at(ce::ResourceId{"users"}).operations.at("create").bodyTemplate;
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, R"({"a":"{{$.base64.encode("x")}}","b":"say \"hi\""})");
}
