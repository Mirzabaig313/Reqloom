// Direct (non-LLM) Thunder Client collection importer tests. Each fails on the
// parent commit (importFromThunderClient did not exist).

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
    ASSERT_TRUE(create->provenance.has_value());
    EXPECT_EQ(create->provenance->source, ce::Provenance::Source::ThunderClientImport);
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

// File fields carrying paths from the exporting machine keep only their base
// name (with a warning); a text field starting with `@` is escaped as `@@`.
TEST(ImportFromThunderClient, file_field_paths_are_reduced_to_base_names_with_warnings) {
    ScratchDir scratch;
    const auto file = scratch.write("uploads.json", R"JSON(
[
  {
    "_id": "col_1", "colName": "Uploads API",
    "folders": [ { "_id": "fld_1", "name": "Uploads", "containerId": "" } ],
    "requests": [
      {
        "_id": "req_1", "colId": "col_1", "containerId": "fld_1", "name": "Upload",
        "method": "POST", "url": "https://api.example.com/up",
        "body": {
          "type": "form",
          "form": [ { "name": "sneaky", "value": "@/etc/passwd" } ],
          "files": [
            { "name": "unix", "value": "/Users/x/.ssh/id_rsa" },
            { "name": "win", "value": "C:\\Users\\x\\a.png" },
            { "name": "rel", "value": "../../secret.txt" },
            { "name": "bare", "value": "photo.png" }
          ]
        }
      }
    ]
  }
]
)JSON");

    auto outcome = ce::importFromThunderClient(file, scratch.path());
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

// A Thunder Client `{id}` path parameter (value in an `isPath` param)
// becomes the shared `{{resource.id}}` reference; request auth warns once,
// `inherit` and `none` do not, and collection `settings.auth` warns once.
TEST(ImportFromThunderClient, path_parameter_and_auth_are_handled_like_other_importers) {
    ScratchDir scratch;
    const auto file = scratch.write("tc_auth.json", R"JSON(
[ { "_id": "c1", "colName": "Users API", "settings": { "auth": { "type": "bearer" } },
    "folders": [ { "_id": "f1", "name": "Users", "containerId": "" } ],
    "requests": [
      { "_id": "r1", "containerId": "f1", "name": "Get User", "method": "GET",
        "url": "https://api.example.com/users/{id}",
        "params": [ { "name": "id", "value": "7", "isPath": true } ],
        "auth": { "type": "inherit" } },
      { "_id": "r2", "containerId": "f1", "name": "Own Auth", "method": "GET",
        "url": "https://api.example.com/me", "auth": { "type": "basic" } },
      { "_id": "r3", "containerId": "f1", "name": "No Auth", "method": "GET",
        "url": "https://api.example.com/open", "auth": { "type": "none" } }
    ] } ]
)JSON");
    auto outcome = ce::importFromThunderClient(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* get = findOp(outcome->project, "users.get_user");
    ASSERT_NE(get, nullptr);
    EXPECT_EQ(get->pathTemplate, "/users/{{users.id}}");
    EXPECT_FALSE(get->queryParams.contains("id"));
    const auto& w = outcome->warnings;
    EXPECT_NE(w.find("`7`"), std::string::npos) << w;
    EXPECT_EQ(countOf(w, "auth was not imported"), 2u) << w;
    EXPECT_EQ(countOf(w, "collection `Users API`: `bearer` auth was not imported"), 1u) << w;
    EXPECT_EQ(countOf(w, "operation users.own_auth: `basic` auth was not imported"), 1u) << w;
}

// Reqloom sends a text-only form urlencoded unless the operation says multipart. Thunder
// Client names a multipart body `formdata` and an urlencoded one `formencoded`.
TEST(ImportFromThunderClient, text_only_formdata_stays_multipart) {
    ScratchDir scratch;
    const auto file = scratch.write("forms.json", R"JSON(
[
  {
    "_id": "col_1", "colName": "Forms",
    "folders": [ { "_id": "fld_1", "name": "Forms", "containerId": "" } ],
    "requests": [
      { "_id": "req_1", "colId": "col_1", "containerId": "fld_1", "name": "Multi",
        "method": "POST", "url": "https://api.example.com/m",
        "body": { "type": "formdata", "form": [ { "name": "a", "value": "1" } ] } },
      { "_id": "req_2", "colId": "col_1", "containerId": "fld_1", "name": "Encoded",
        "method": "POST", "url": "https://api.example.com/e",
        "body": { "type": "formencoded", "form": [ { "name": "a", "value": "1" } ] } }
    ]
  }
]
)JSON");
    auto outcome = ce::importFromThunderClient(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* multi = findOp(outcome->project, "forms.multi");
    ASSERT_NE(multi, nullptr);
    ASSERT_TRUE(multi->bodyForm.has_value());
    EXPECT_EQ(multi->headers.at("Content-Type"), "multipart/form-data");
    const auto* encoded = findOp(outcome->project, "forms.encoded");
    ASSERT_NE(encoded, nullptr);
    EXPECT_TRUE(encoded->headers.empty());
}
