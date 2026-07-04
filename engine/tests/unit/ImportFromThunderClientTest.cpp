// Direct (non-LLM) Thunder Client collection importer tests. Each fails on the
// parent commit (importFromThunderClient did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-thunder-import");
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

// The "Export Collection" shape: a JSON array with one collection object that
// carries a top-level folder ("Orders") and two requests bound to it via
// containerId. The POST has a JSON body; the GET has a query param.
constexpr const char* kExport = R"JSON(
[
  {
    "_id": "col_1",
    "colName": "Orders API",
    "folders": [ { "_id": "fld_1", "name": "Orders", "containerId": "" } ],
    "requests": [
      {
        "_id": "req_1", "colId": "col_1", "containerId": "fld_1", "name": "Create Order",
        "method": "POST", "url": "https://api.example.com/api/v1/orders",
        "headers": [ { "name": "X-Api-Key", "value": "{{apiKey}}" } ],
        "body": { "type": "json", "raw": "{\"item\":\"widget\"}" }
      },
      {
        "_id": "req_2", "colId": "col_1", "containerId": "fld_1", "name": "List Orders",
        "method": "GET", "url": "https://api.example.com/api/v1/orders",
        "params": [ { "name": "status", "value": "open", "isPath": false } ]
      }
    ]
  }
]
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

TEST(ImportFromThunderClient, rejects_non_thunder_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0","paths":{}})");
    auto outcome = ce::importFromThunderClient(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("Thunder Client"), std::string::npos);
}

TEST(ImportFromThunderClient, folder_becomes_resource_and_requests_become_operations) {
    ScratchDir scratch;
    const auto file = scratch.write("thunder-collection_orders.json", kExport);

    auto outcome = ce::importFromThunderClient(file, scratch.path());
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

TEST(ImportFromThunderClient, host_becomes_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.json", kExport);

    auto outcome = ce::importFromThunderClient(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromThunderClient, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-thunder-escape.json";
    {
        std::ofstream out{outside};
        out << kExport;
    }
    auto outcome = ce::importFromThunderClient(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}
