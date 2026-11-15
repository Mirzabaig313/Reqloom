// Bruno collection importer (directory of .bru block-DSL files). Each test
// fails on the parent commit (importFromBruno did not exist).

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <support/ImportFileRefChecks.h>
#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = reqloom::tests::uniqueTempPath("reqloom-bruno-import");
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

    void write(const fs::path& rel, const std::string& body) {
        const auto full = path_ / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out{full};
        out << body;
    }

private:
    fs::path path_;
};

// A Bruno collection: bruno.json marker, an "Orders" folder with two .bru
// requests, and an environment with baseUrl + apiKey.
void writeCollection(ScratchDir& s) {
    s.write("bruno.json", R"({"version":"1","name":"Orders API","type":"collection"})");
    s.write("environments/Local.bru",
            "vars {\n"
            "  baseUrl: https://api.example.com\n"
            "  apiKey: abc123\n"
            "}\n");
    s.write("Orders/Create Order.bru",
            "meta {\n"
            "  name: Create Order\n"
            "  type: http\n"
            "}\n"
            "\n"
            "post {\n"
            "  url: {{baseUrl}}/api/v1/orders\n"
            "  body: json\n"
            "  auth: none\n"
            "}\n"
            "\n"
            "headers {\n"
            "  X-Api-Key: {{apiKey}}\n"
            "}\n"
            "\n"
            "body:json {\n"
            "  {\n"
            "    \"item\": \"widget\"\n"
            "  }\n"
            "}\n");
    s.write("Orders/List Orders.bru",
            "meta {\n"
            "  name: List Orders\n"
            "}\n"
            "\n"
            "get {\n"
            "  url: {{baseUrl}}/api/v1/orders\n"
            "}\n"
            "\n"
            "query {\n"
            "  status: open\n"
            "}\n");
}

const ce::Operation* findOp(const ce::Project& p, const std::string& id) {
    for (const auto& [resId, res] : p.resources) {
        for (const auto& [opName, op] : res.operations) {
            if (op.id.value == id) {
                return &op;
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST(ImportFromBruno, rejects_directory_without_bru_files) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"name":"Empty"})");
    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("no request"), std::string::npos);
}

TEST(ImportFromBruno, folder_becomes_resource_and_bru_files_become_operations) {
    ScratchDir scratch;
    writeCollection(scratch);

    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    EXPECT_EQ(project.name, "Orders API");
    ASSERT_TRUE(project.resources.contains(ce::ResourceId{"orders"}));
    EXPECT_EQ(project.resources.at(ce::ResourceId{"orders"}).operations.size(), 2u);

    const auto* create = findOp(project, "orders.create_order");
    ASSERT_NE(create, nullptr);
    EXPECT_EQ(create->method, ce::HttpMethod::Post);
    EXPECT_EQ(create->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(create->bodyTemplate.has_value());
    EXPECT_NE(create->bodyTemplate->find("widget"), std::string::npos);
    ASSERT_TRUE(create->headers.contains("X-Api-Key"));
    EXPECT_EQ(create->headers.at("X-Api-Key"), "{{env.apiKey}}");
    ASSERT_TRUE(create->provenance.has_value());
    EXPECT_EQ(create->provenance->source, ce::Provenance::Source::BrunoImport);
}

TEST(ImportFromBruno, environment_vars_seed_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    writeCollection(scratch);

    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    // The Bruno environment `Local` keeps its own name and is the project default.
    EXPECT_EQ(project.defaultEnvironment, "local");
    ASSERT_TRUE(project.environments.contains("local"));
    EXPECT_EQ(project.environments.at("local").at("baseUrl"), "https://api.example.com");
    EXPECT_EQ(project.environments.at("local").at("apiKey"), "abc123");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromBruno, accepts_bruno_json_path_and_derives_root) {
    ScratchDir scratch;
    writeCollection(scratch);

    // Point at bruno.json; the importer derives the collection root from it.
    auto outcome = ce::importFromBruno(scratch.path() / "bruno.json", scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome->project.resources.contains(ce::ResourceId{"orders"}));
}

// Bruno writes multipart file fields as `@file(<path>)`. Paths from the
// exporting machine keep only their base name (with a warning); a text field
// starting with `@` is escaped as `@@`.
TEST(ImportFromBruno, file_field_paths_are_reduced_to_base_names_with_warnings) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"version":"1","name":"Uploads API","type":"collection"})");
    scratch.write("Uploads/Upload.bru",
                  "meta {\n"
                  "  name: Upload\n"
                  "}\n"
                  "\n"
                  "post {\n"
                  "  url: https://api.example.com/up\n"
                  "  body: multipartForm\n"
                  "}\n"
                  "\n"
                  "body:multipart-form {\n"
                  "  unix: @file(/Users/x/.ssh/id_rsa)\n"
                  "  win: @file(C:\\Users\\x\\a.png)\n"
                  "  rel: @file(../../secret.txt)\n"
                  "  bare: @file(photo.png)\n"
                  "  sneaky: @/etc/passwd\n"
                  "}\n");

    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* op = findOp(outcome->project, "uploads.upload");
    ASSERT_NE(op, nullptr);
    ASSERT_TRUE(op->bodyForm.has_value());
    reqloom::tests::expectImportedFileRefsSanitised(
        *op->bodyForm, outcome->warnings, "uploads.upload");
}

namespace {

std::size_t countOf(const std::string& haystack, std::string_view needle) {
    std::size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// `params:path` used to be parsed as a block named `params` and turned into
// query parameters (`/users/:id?id=123`). The path parameter becomes the
// shared `{{resource.id}}` reference and `params:query` stays a query.
TEST(ImportFromBruno, params_path_becomes_path_parameter_not_query) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"version":"1","name":"Users","type":"collection"})");
    scratch.write("Users/Get User.bru",
                  "meta {\n  name: Get User\n}\n\n"
                  "get {\n  url: https://api.example.com/users/:id?verbose=true\n}\n\n"
                  "params:query {\n  verbose: true\n}\n\n"
                  "params:path {\n  id: 123\n}\n");
    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* op = findOp(outcome->project, "users.get_user");
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->pathTemplate, "/users/{{users.id}}");
    EXPECT_FALSE(op->queryParams.contains("id"));
    EXPECT_EQ(op->queryParams.at("verbose"), "true");
    EXPECT_NE(outcome->warnings.find("`123`"), std::string::npos) << outcome->warnings;
}

// Bruno disables a line with a leading `~`; it used to be imported as an
// enabled header named `~X-Debug`.
TEST(ImportFromBruno, tilde_disabled_entries_are_skipped) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"version":"1","name":"Api","type":"collection"})");
    scratch.write("Ping.bru",
                  "get {\n  url: https://api.example.com/ping\n}\n\n"
                  "headers {\n  ~X-Debug: 1\n  X-Live: 1\n}\n\n"
                  "query {\n  ~trace: on\n}\n");
    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* op = findOp(outcome->project, "api.ping");
    ASSERT_NE(op, nullptr);
    EXPECT_FALSE(op->headers.contains("~X-Debug"));
    EXPECT_FALSE(op->headers.contains("X-Debug"));
    EXPECT_EQ(op->headers.at("X-Live"), "1");
    EXPECT_TRUE(op->queryParams.empty());
}

// Request auth (`auth: bearer`) and collection.bru auth each warn once;
// `auth: none` and `auth: inherit` do not.
TEST(ImportFromBruno, auth_warnings_once_per_scope) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"version":"1","name":"Api","type":"collection"})");
    scratch.write("collection.bru", "auth {\n  mode: basic\n}\n");
    scratch.write("A.bru", "get {\n  url: https://api.example.com/a\n  auth: bearer\n}\n");
    scratch.write("B.bru", "get {\n  url: https://api.example.com/b\n  auth: inherit\n}\n");
    scratch.write("C.bru", "get {\n  url: https://api.example.com/c\n  auth: none\n}\n");
    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& w = outcome->warnings;
    EXPECT_EQ(countOf(w, "auth was not imported"), 2u) << w;
    EXPECT_EQ(countOf(w, "collection `Api`: `basic` auth was not imported"), 1u) << w;
    EXPECT_EQ(countOf(w, "operation api.a: `bearer` auth was not imported"), 1u) << w;
}

// Each environments/*.bru file becomes its own environment (they used to be
// merged last-wins in directory order); the first by name is the default.
TEST(ImportFromBruno, each_environment_file_is_kept_separately) {
    ScratchDir scratch;
    scratch.write("bruno.json", R"({"version":"1","name":"Api","type":"collection"})");
    scratch.write("environments/Local.bru", "vars {\n  host: http://localhost:3000\n}\n");
    scratch.write("environments/Prod.bru", "vars {\n  host: https://prod.example.com\n}\n");
    scratch.write("Ping.bru", "get {\n  url: {{host}}/ping\n}\n");
    auto outcome = ce::importFromBruno(scratch.path(), scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& envs = outcome->project.environments;
    EXPECT_EQ(outcome->project.defaultEnvironment, "local");
    ASSERT_TRUE(envs.contains("local"));
    ASSERT_TRUE(envs.contains("prod"));
    EXPECT_EQ(envs.at("local").at("host"), "http://localhost:3000");
    EXPECT_EQ(envs.at("prod").at("host"), "https://prod.example.com");
    EXPECT_EQ(envs.at("local").at("baseUrl"), "http://localhost:3000");
    EXPECT_EQ(envs.at("prod").at("baseUrl"), "https://prod.example.com");
}
