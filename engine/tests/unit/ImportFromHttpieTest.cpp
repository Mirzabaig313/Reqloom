// Direct (non-LLM) HTTPie export importer tests. Each fails on the parent
// commit (importFromHttpie did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-httpie-import");
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

// A workspace export: one environment (default) + one collection ("Orders")
// with a POST (JSON text body + token header) and a GET (query param).
constexpr const char* kExport = R"JSON(
{
  "meta": { "format": "httpie", "version": "2024.1.0", "contentType": "workspace" },
  "entry": {
    "name": "Orders API",
    "collections": [
      {
        "name": "Orders",
        "auth": { "type": "none" },
        "requests": [
          {
            "name": "Create Order", "method": "POST", "url": "{{baseUrl}}/api/v1/orders",
            "headers": [ { "name": "X-Api-Key", "value": "{{apiKey}}", "enabled": true } ],
            "queryParams": [], "pathParams": [],
            "auth": { "type": "inherited" },
            "body": { "type": "text", "text": { "format": "application/json", "value": "{\"item\":\"widget\"}" } }
          },
          {
            "name": "List Orders", "method": "GET", "url": "{{baseUrl}}/api/v1/orders",
            "headers": [], "pathParams": [],
            "queryParams": [ { "name": "status", "value": "open", "enabled": true } ],
            "auth": { "type": "none" },
            "body": { "type": "none" }
          }
        ]
      }
    ],
    "environments": [
      {
        "isDefault": true, "isLocalOnly": false, "name": "Local",
        "variables": [
          { "name": "baseUrl", "value": "https://api.example.com", "isSecret": false },
          { "name": "apiKey", "value": "abc123", "isSecret": false }
        ]
      }
    ],
    "drafts": []
  }
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

TEST(ImportFromHttpie, rejects_non_httpie_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0","paths":{}})");
    auto outcome = ce::importFromHttpie(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("HTTPie"), std::string::npos);
}

TEST(ImportFromHttpie, collection_becomes_resource_and_requests_become_operations) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.httpie.json", kExport);

    auto outcome = ce::importFromHttpie(file, scratch.path());
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
}

TEST(ImportFromHttpie, default_environment_seeds_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders2.json", kExport);

    auto outcome = ce::importFromHttpie(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");
    EXPECT_EQ(project.environments.at("default").at("apiKey"), "abc123");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromHttpie, importAny_routes_httpie_export) {
    ScratchDir scratch;
    const auto file = scratch.write("orders3.json", kExport);
    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome->project.resources.contains(ce::ResourceId{"orders"}));
}

TEST(ImportFromHttpie, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-httpie-escape.json";
    {
        std::ofstream out{outside};
        out << kExport;
    }
    auto outcome = ce::importFromHttpie(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}
