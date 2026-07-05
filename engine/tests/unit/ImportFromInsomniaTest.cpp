// Direct (non-LLM) Insomnia v4 importer tests. Each fails on the parent commit
// (importFromInsomnia did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-insomnia-import");
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
};

// A representative v4 export: workspace → one request group ("Orders") holding
// two requests, plus a base environment with base_url. The POST body uses the
// {{ _.var }} template form; the GET carries a query parameter.
constexpr const char* kExport = R"JSON(
{
  "_type": "export",
  "__export_format": 4,
  "__export_source": "insomnia.desktop.app:v2023.5.8",
  "resources": [
    { "_id": "wrk_1", "_type": "workspace", "name": "Orders API" },
    { "_id": "env_1", "_type": "environment", "parentId": "wrk_1", "name": "Base",
      "data": { "base_url": "https://api.example.com", "api_key": "abc123" } },
    { "_id": "fld_1", "_type": "request_group", "parentId": "wrk_1", "name": "Orders" },
    {
      "_id": "req_1", "_type": "request", "parentId": "fld_1", "name": "Create Order",
      "method": "POST",
      "url": "{{ _.base_url }}/api/v1/orders",
      "headers": [ { "name": "X-Api-Key", "value": "{{ _.api_key }}" } ],
      "body": { "mimeType": "application/json", "text": "{\"item\":\"widget\"}" }
    },
    {
      "_id": "req_2", "_type": "request", "parentId": "fld_1", "name": "List Orders",
      "method": "GET",
      "url": "{{ _.base_url }}/api/v1/orders",
      "parameters": [ { "name": "status", "value": "open" } ]
    }
  ]
}
)JSON";

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

TEST(ImportFromInsomnia, rejects_non_insomnia_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0","paths":{}})");
    auto outcome = ce::importFromInsomnia(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("Insomnia"), std::string::npos);
}

TEST(ImportFromInsomnia, request_group_becomes_resource_and_requests_become_operations) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.insomnia.json", kExport);

    auto outcome = ce::importFromInsomnia(file, scratch.path());
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
    // {{ _.api_key }} was rewritten to the env scope so it resolves at run time.
    ASSERT_TRUE(create->headers.contains("X-Api-Key"));
    EXPECT_EQ(create->headers.at("X-Api-Key"), "{{env.api_key}}");
    ASSERT_TRUE(create->provenance.has_value());
    EXPECT_EQ(create->provenance->source, ce::Provenance::Source::InsomniaImport);
}

TEST(ImportFromInsomnia, environment_data_seeds_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.json", kExport);

    auto outcome = ce::importFromInsomnia(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");
    EXPECT_EQ(project.environments.at("default").at("api_key"), "abc123");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromInsomnia, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-insomnia-escape.insomnia.json";
    {
        std::ofstream out{outside};
        out << kExport;
    }
    auto outcome = ce::importFromInsomnia(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}
