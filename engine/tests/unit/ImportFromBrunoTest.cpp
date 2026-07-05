// Bruno collection importer (directory of .bru block-DSL files). Each test
// fails on the parent commit (importFromBruno did not exist).

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <string>

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

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");
    EXPECT_EQ(project.environments.at("default").at("apiKey"), "abc123");

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
