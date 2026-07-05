// Direct (non-LLM) Hoppscotch collection importer tests. Each fails on the
// parent commit (importFromHoppscotch did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-hoppscotch-import");
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

// A representative collection: one top-level folder ("Orders") with two
// requests. The POST uses a full-host endpoint + JSON body and an <<apiKey>>
// header token; the GET carries a query param.
constexpr const char* kExport = R"JSON(
{
  "v": 1,
  "name": "Orders API",
  "folders": [
    {
      "v": 1,
      "name": "Orders",
      "folders": [],
      "requests": [
        {
          "v": "1", "name": "Create Order", "method": "POST",
          "endpoint": "https://api.example.com/api/v1/orders",
          "headers": [ { "key": "X-Api-Key", "value": "<<apiKey>>", "active": true } ],
          "params": [],
          "body": { "contentType": "application/json", "body": "{\"item\":\"widget\"}" }
        },
        {
          "v": "1", "name": "List Orders", "method": "GET",
          "endpoint": "https://api.example.com/api/v1/orders",
          "headers": [],
          "params": [ { "key": "status", "value": "open", "active": true } ],
          "body": { "contentType": null, "body": null }
        }
      ]
    }
  ],
  "requests": []
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

TEST(ImportFromHoppscotch, rejects_non_hoppscotch_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0"})");
    auto outcome = ce::importFromHoppscotch(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("Hoppscotch"), std::string::npos);
}

TEST(ImportFromHoppscotch, folder_becomes_resource_and_requests_become_operations) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.json", kExport);

    auto outcome = ce::importFromHoppscotch(file, scratch.path());
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
    // <<apiKey>> was rewritten to the env scope so it resolves at run time.
    ASSERT_TRUE(create->headers.contains("X-Api-Key"));
    EXPECT_EQ(create->headers.at("X-Api-Key"), "{{env.apiKey}}");
}

TEST(ImportFromHoppscotch, host_becomes_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders2.json", kExport);

    auto outcome = ce::importFromHoppscotch(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromHoppscotch, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-hoppscotch-escape.json";
    {
        std::ofstream out{outside};
        out << kExport;
    }
    auto outcome = ce::importFromHoppscotch(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}
