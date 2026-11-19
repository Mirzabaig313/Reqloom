// SchemaErrorPositionTest — a schema error says where it is and what would fix it (PRD FR-15.5):
// `ReqloomError::file`, `line`, `column`, `reference` and `fix`, for `reqloom lint`. The line is
// the line of the field holding the offending reference, not only the operation's key.

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class Scratch {
public:
    Scratch() : path_(reqloom::tests::uniqueTempPath("reqloom-schema-pos")) {
        fs::create_directories(path_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    fs::path write(const std::string& name, const std::string& body) {
        const auto full = path_ / name;
        fs::create_directories(full.parent_path());
        std::ofstream{full} << body;
        return full;
    }

private:
    fs::path path_;
};

[[nodiscard]] ce::ReqloomError loadError(const fs::path& yaml) {
    auto project = ce::parseProject(yaml);
    EXPECT_FALSE(project.has_value());
    return project ? ce::ReqloomError{} : project.error();
}

}  // namespace

TEST(SchemaErrorPosition, undefined_scope_points_at_the_header_that_uses_it) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"                         // 1
                                    "name: P\n"                            // 2
                                    "environment:\n"                       // 3
                                    "  baseUrl: http://localhost:0\n"      // 4
                                    "resources:\n"                         // 5
                                    "  order:\n"                           // 6
                                    "    operations:\n"                    // 7
                                    "      get:\n"                         // 8
                                    "        method: GET\n"                // 9
                                    "        path: /orders\n"              // 10
                                    "        headers:\n"                   // 11
                                    "          Accept: json\n"             // 12
                                    "          X-Who: \"{{ghost.id}}\"\n"  // 13
    );
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(error.file, yaml.string());
    EXPECT_EQ(error.line, 13);
    EXPECT_EQ(error.reference, "{{ghost.id}}");
    EXPECT_NE(error.fix.find("order"), std::string::npos) << error.fix;
    // The detail is unchanged: prefixed with the operation's own line.
    EXPECT_TRUE(error.detail.starts_with(yaml.string() + ":8: ")) << error.detail;
}

TEST(SchemaErrorPosition, unknown_actor_points_at_the_actor_line_and_lists_the_known_ones) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "environment:\n"
                                    "  baseUrl: http://localhost:0\n"
                                    "actors:\n"
                                    "  admin:\n"
                                    "    description: x\n"
                                    "resources:\n"
                                    "  order:\n"
                                    "    operations:\n"
                                    "      get:\n"             // 11
                                    "        method: GET\n"    // 12
                                    "        path: /orders\n"  // 13
                                    "        actor: amdin\n"   // 14
    );
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(error.line, 14);
    EXPECT_EQ(error.reference, "amdin");
    EXPECT_NE(error.fix.find("admin"), std::string::npos) << error.fix;
}

TEST(SchemaErrorPosition, missing_depends_on_target_points_at_the_list_item) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "environment:\n"
                                    "  baseUrl: http://localhost:0\n"
                                    "resources:\n"
                                    "  order:\n"
                                    "    operations:\n"
                                    "      create:\n"
                                    "        method: POST\n"
                                    "        path: /orders\n"
                                    "      get:\n"
                                    "        method: GET\n"
                                    "        path: /orders/1\n"
                                    "        depends_on:\n"       // 14
                                    "          - order.create\n"  // 15
                                    "          - order.nope\n"    // 16
    );
    const auto error = loadError(yaml);
    EXPECT_EQ(error.line, 16);
    EXPECT_EQ(error.reference, "order.nope");
    EXPECT_FALSE(error.fix.empty());
}

TEST(SchemaErrorPosition, ambiguous_producer_in_a_sub_file_names_that_file_and_line) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "environment:\n"
                                    "  baseUrl: http://localhost:0\n"
                                    "imports:\n"
                                    "  - resources/*.yaml\n");
    const auto sub = scratch.write("resources/order.yaml",
                                   "name: order\n"                       // 1
                                   "operations:\n"                       // 2
                                   "  create:\n"                         // 3
                                   "    method: POST\n"                  // 4
                                   "    path: /orders\n"                 // 5
                                   "    extract:\n"                      // 6
                                   "      order_id: $.id\n"              // 7
                                   "  copy:\n"                           // 8
                                   "    method: POST\n"                  // 9
                                   "    path: /orders/copy\n"            // 10
                                   "    extract:\n"                      // 11
                                   "      order_id: $.id\n"              // 12
                                   "  get:\n"                            // 13
                                   "    method: GET\n"                   // 14
                                   "    path: /orders\n"                 // 15
                                   "    query_params:\n"                 // 16
                                   "      id: \"{{order.order_id}}\"\n"  // 17
    );
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(error.file, sub.string());
    EXPECT_EQ(error.line, 17);
    EXPECT_EQ(error.reference, "{{order.order_id}}");
    EXPECT_NE(error.fix.find("order.copy"), std::string::npos) << error.fix;
}

TEST(SchemaErrorPosition, a_yaml_syntax_error_carries_line_and_column) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "resources: [unclosed\n");
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::YamlParse);
    EXPECT_EQ(error.file, yaml.string());
    EXPECT_GT(error.line, 0);
    EXPECT_GT(error.column, 0);
}

TEST(SchemaErrorPosition, a_wrong_type_carries_the_position_of_the_value) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "resources:\n"
                                    "  order:\n"
                                    "    operations:\n"
                                    "      get:\n"
                                    "        method: GET\n"
                                    "        path: /x\n"
                                    "        expect_status: [a, b]\n");  // 9
    const auto error = loadError(yaml);
    EXPECT_EQ(error.file, yaml.string());
    EXPECT_EQ(error.line, 9);
    EXPECT_GT(error.column, 0);
}

TEST(SchemaErrorPosition, a_cycle_points_at_one_of_its_operations) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "resources:\n"
                                    "  a:\n"
                                    "    operations:\n"
                                    "      one:\n"
                                    "        path: /1\n"
                                    "        depends_on: [a.two]\n"  // 8
                                    "      two:\n"
                                    "        path: /2\n"
                                    "        depends_on: [a.one]\n");  // 11
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::Cycle);
    EXPECT_EQ(error.file, yaml.string());
    EXPECT_EQ(error.line, 8);
    EXPECT_FALSE(error.fix.empty());
}

TEST(SchemaErrorPosition, load_warnings_carry_their_position_at_the_same_index) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"
                                    "name: P\n"
                                    "colour: blue\n");  // 3
    auto project = ce::parseProject(yaml);
    ASSERT_TRUE(project.has_value()) << project.error().detail;
    ASSERT_EQ(project->loadWarnings.size(), 1U);
    ASSERT_EQ(project->loadWarningSources.size(), 1U);
    EXPECT_EQ(project->loadWarningSources[0].file, yaml.string());
    EXPECT_EQ(project->loadWarningSources[0].line, 3);
    EXPECT_EQ(project->loadWarningSources[0].column, 1);
}

TEST(SchemaErrorPosition, a_cycle_names_its_members_not_the_operations_downstream_of_it) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"                   // 1
                                    "name: P\n"                      // 2
                                    "resources:\n"                   // 3
                                    "  a:\n"                         // 4
                                    "    operations:\n"              // 5
                                    "      x:\n"                     // 6
                                    "        path: /x/{{b.id}}\n"    // 7
                                    "  b:\n"                         // 8
                                    "    operations:\n"              // 9
                                    "      c:\n"                     // 10
                                    "        path: /b\n"             // 11
                                    "        depends_on: [c.c]\n"    // 12
                                    "        extract:\n"             // 13
                                    "          id: $.id\n"           // 14
                                    "  c:\n"                         // 15
                                    "    operations:\n"              // 16
                                    "      c:\n"                     // 17
                                    "        path: /c\n"             // 18
                                    "        depends_on: [b.c]\n");  // 19
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::Cycle);
    EXPECT_EQ(error.line, 12) << error.detail;
    EXPECT_EQ(error.reference.find("a.x"), std::string::npos) << error.reference;
    EXPECT_EQ(error.detail.find("a.x"), std::string::npos) << error.detail;
    EXPECT_NE(error.reference.find("b.c"), std::string::npos) << error.reference;
    EXPECT_NE(error.reference.find("c.c"), std::string::npos) << error.reference;
}

TEST(SchemaErrorPosition, a_bad_reference_in_inherited_default_auth_points_at_the_default) {
    Scratch scratch;
    const auto yaml = scratch.write("reqloom.yaml",
                                    "version: 1\n"                    // 1
                                    "name: P\n"                       // 2
                                    "auth:\n"                         // 3
                                    "  type: bearer\n"                // 4
                                    "  token: \"{{ghost.token}}\"\n"  // 5
                                    "imports:\n"                      // 6
                                    "  - resources/*.yaml\n");        // 7
    scratch.write("resources/item.yaml",
                  "name: item\noperations:\n  read:\n    path: /x\n    auth:\n"
                  "      type: inherit\n");
    const auto error = loadError(yaml);
    EXPECT_EQ(error.code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(error.file, yaml.string());
    EXPECT_EQ(error.line, 3);
}
