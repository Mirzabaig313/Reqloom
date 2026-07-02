// Direct (non-LLM) Postman Collection v2.1 importer tests. Each fails on the
// parent commit (importFromPostman did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-postman-import");
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

// A small but representative v2.1 collection: one folder with two requests
// (a POST with a JSON body and a GET with a path + query), collection-level
// variables, and a {{baseUrl}} URL.
constexpr const char* kCollection = R"JSON(
{
  "info": { "name": "Orders API", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "variable": [
    { "key": "baseUrl", "value": "https://api.example.com" },
    { "key": "apiKey", "value": "abc123" }
  ],
  "item": [
    {
      "name": "Orders",
      "item": [
        {
          "name": "Create Order",
          "request": {
            "method": "POST",
            "header": [ { "key": "X-Api-Key", "value": "{{apiKey}}" } ],
            "url": { "raw": "{{baseUrl}}/api/v1/orders", "path": ["api","v1","orders"] },
            "body": { "mode": "raw", "raw": "{\"item\":\"widget\"}", "options": { "raw": { "language": "json" } } }
          }
        },
        {
          "name": "List Orders",
          "request": {
            "method": "GET",
            "url": {
              "raw": "{{baseUrl}}/api/v1/orders?status=open",
              "path": ["api","v1","orders"],
              "query": [ { "key": "status", "value": "open" } ]
            }
          }
        }
      ]
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

TEST(ImportFromPostman, rejects_non_postman_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0","paths":{}})");
    auto outcome = ce::importFromPostman(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("Postman"), std::string::npos);
}

TEST(ImportFromPostman, folder_becomes_resource_and_requests_become_operations) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.postman_collection.json", kCollection);

    auto outcome = ce::importFromPostman(file, scratch.path());
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
    // Bare {{apiKey}} was rewritten to the env scope so it resolves at run time.
    ASSERT_TRUE(create->headers.contains("X-Api-Key"));
    EXPECT_EQ(create->headers.at("X-Api-Key"), "{{env.apiKey}}");
    ASSERT_TRUE(create->provenance.has_value());
    EXPECT_EQ(create->provenance->source, ce::Provenance::Source::PostmanImport);
}

TEST(ImportFromPostman, host_becomes_baseurl_and_query_is_kept) {
    ScratchDir scratch;
    const auto file = scratch.write("orders.json", kCollection);

    auto outcome = ce::importFromPostman(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    ASSERT_TRUE(project.environments.contains("default"));
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.com");

    const auto* list = findOp(project, "orders.list_orders");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->pathTemplate, "/api/v1/orders");
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "open");
}

TEST(ImportFromPostman, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    // Write the collection one level up, then try to import it with the scratch
    // dir as the containment root — must be refused.
    const auto outside =
        scratch.path().parent_path() / "reqloom-postman-escape.postman_collection.json";
    {
        std::ofstream out{outside};
        out << kCollection;
    }
    auto outcome = ce::importFromPostman(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}

// A collection with a multipart formdata body: one text field and one file
// field carrying a source path.
constexpr const char* kMultipartCollection = R"JSON(
{
  "info": { "name": "Uploads", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    {
      "name": "Upload Avatar",
      "request": {
        "method": "POST",
        "url": { "raw": "https://api.example.com/avatar", "path": ["avatar"] },
        "body": {
          "mode": "formdata",
          "formdata": [
            { "key": "caption", "value": "hi", "type": "text" },
            { "key": "avatar", "type": "file", "src": "/tmp/pic.png" },
            { "key": "empty", "type": "file", "src": [] }
          ]
        }
      }
    }
  ]
}
)JSON";

TEST(ImportFromPostman, formdata_file_fields_become_at_path_references) {
    ScratchDir scratch;
    const auto file = scratch.write("uploads.json", kMultipartCollection);

    auto outcome = ce::importFromPostman(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;

    const auto* op = findOp(outcome->project, "uploads.upload_avatar");
    ASSERT_NE(op, nullptr);
    ASSERT_TRUE(op->bodyForm.has_value());
    const auto& form = *op->bodyForm;
    // Text field kept verbatim; file field mapped to the `@path` convention so
    // the engine sends multipart/form-data with the file attached.
    EXPECT_EQ(form.at("caption"), "hi");
    EXPECT_EQ(form.at("avatar"), "@/tmp/pic.png");
    // A file field with no source path is preserved as an `@` placeholder to be
    // filled before running — not silently dropped.
    EXPECT_EQ(form.at("empty"), "@");

    // An empty file field is the normal Postman case (exports carry no path),
    // so it produces no warning — the editor shows it as a "Choose a file…"
    // slot. The only thing worth flagging would be genuinely lossy cases.
    EXPECT_TRUE(outcome->warnings.empty()) << outcome->warnings;
}
