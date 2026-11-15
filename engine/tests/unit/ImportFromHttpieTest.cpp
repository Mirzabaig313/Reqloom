// Direct (non-LLM) HTTPie export importer tests. Each fails on the parent
// commit (importFromHttpie did not exist).

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
    ASSERT_TRUE(create->provenance.has_value());
    EXPECT_EQ(create->provenance->source, ce::Provenance::Source::HttpieImport);
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

    // The HTTPie default environment `Local` keeps its own name and is the project default.
    EXPECT_EQ(project.defaultEnvironment, "local");
    ASSERT_TRUE(project.environments.contains("local"));
    EXPECT_EQ(project.environments.at("local").at("baseUrl"), "https://api.example.com");
    EXPECT_EQ(project.environments.at("local").at("apiKey"), "abc123");

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

TEST(ImportFromHttpie, importAny_does_not_misroute_files_merely_mentioning_httpie) {
    ScratchDir scratch;
    // An OpenAPI doc whose description mentions "httpie" must NOT route to the
    // HTTPie importer (regression: the sniff used to match a bare "httpie").
    const auto file = scratch.write("openapi.yaml", R"YAML(
openapi: "3.0.3"
info:
  title: Ping API
  description: Try it with httpie or curl.
paths:
  /ping:
    get:
      responses:
        "200": { description: ok }
)YAML");

    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->project.name, "Ping API");
    ASSERT_TRUE(outcome->project.resources.contains(ce::ResourceId{"ping"}));
}

TEST(ImportFromHttpie, importAny_reports_json_schema_files_clearly) {
    ScratchDir scratch;
    // A JSON Schema document (defines a format) is not an export — importAny
    // should say so instead of a cryptic "not OpenAPI" error.
    const auto file = scratch.write("httpie_schema.json", R"JSON(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "HTTPie Export Format",
  "type": "object",
  "definitions": { "HTTPieMeta": { "properties": { "format": { "const": "httpie" } } } }
}
)JSON");

    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("JSON Schema document"), std::string::npos);
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

// File fields carrying paths from the exporting machine keep only their base
// name (with a warning); a text field starting with `@` is escaped as `@@`.
TEST(ImportFromHttpie, file_field_paths_are_reduced_to_base_names_with_warnings) {
    ScratchDir scratch;
    const auto file = scratch.write("uploads.json", R"JSON(
{
  "meta": { "format": "httpie", "version": "2024.1.0", "contentType": "workspace" },
  "entry": {
    "name": "Uploads API",
    "collections": [
      {
        "name": "Uploads",
        "requests": [
          {
            "name": "Upload", "method": "POST", "url": "https://api.example.com/up",
            "body": { "type": "form", "form": { "isMultipart": true, "fields": [
              { "name": "unix", "type": "file", "file": { "name": "/Users/x/.ssh/id_rsa" } },
              { "name": "win", "type": "file", "file": { "name": "C:\\Users\\x\\a.png" } },
              { "name": "rel", "type": "file", "file": { "name": "../../secret.txt" } },
              { "name": "bare", "type": "file", "file": { "name": "photo.png" } },
              { "name": "sneaky", "type": "text", "value": "@/etc/passwd" }
            ] } }
          }
        ]
      }
    ],
    "environments": []
  }
}
)JSON");

    auto outcome = ce::importFromHttpie(file, scratch.path());
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

// Each HTTPie environment becomes its own Reqloom environment. The default
// one used to be overwritten key by key by a later environment (Staging's
// `host` replaced the default Prod `host`).
TEST(ImportFromHttpie, each_environment_is_kept_separately) {
    ScratchDir scratch;
    const auto file = scratch.write("envs.json", R"JSON(
{ "meta": { "format": "httpie", "contentType": "workspace" },
  "entry": { "name": "Envs", "drafts": [],
    "collections": [ { "name": "Api", "auth": { "type": "none" }, "requests": [
      { "name": "Ping", "method": "GET", "url": "{{host}}/ping", "auth": { "type": "none" } } ] } ],
    "environments": [
      { "name": "Prod", "isDefault": true, "isLocalOnly": false,
        "variables": [ { "name": "host", "value": "https://prod.example.com", "isSecret": false } ] },
      { "name": "Staging", "isDefault": false, "isLocalOnly": false,
        "variables": [ { "name": "host", "value": "https://staging.example.com", "isSecret": false } ] }
    ] } }
)JSON");
    auto outcome = ce::importFromHttpie(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& envs = outcome->project.environments;
    EXPECT_EQ(outcome->project.defaultEnvironment, "prod");
    ASSERT_TRUE(envs.contains("prod"));
    ASSERT_TRUE(envs.contains("staging"));
    EXPECT_EQ(envs.at("prod").at("host"), "https://prod.example.com");
    EXPECT_EQ(envs.at("staging").at("host"), "https://staging.example.com");
    EXPECT_EQ(envs.at("prod").at("baseUrl"), "https://prod.example.com");
    EXPECT_EQ(envs.at("staging").at("baseUrl"), "https://staging.example.com");
}

// A request with `inherited` auth uses its collection's auth, which is
// reported once at collection level (it used to be dropped silently).
TEST(ImportFromHttpie, collection_auth_is_reported_once) {
    ScratchDir scratch;
    const auto file = scratch.write("cauth.json", R"JSON(
{ "meta": { "format": "httpie", "contentType": "collection" },
  "entry": { "name": "Api", "auth": { "type": "bearer" }, "requests": [
    { "name": "A", "method": "GET", "url": "https://api.example.com/a", "auth": { "type": "inherited" } },
    { "name": "B", "method": "GET", "url": "https://api.example.com/b", "auth": { "type": "inherited" } } ] } }
)JSON");
    auto outcome = ce::importFromHttpie(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& w = outcome->warnings;
    EXPECT_EQ(countOf(w, "auth was not imported"), 1u) << w;
    EXPECT_EQ(countOf(w, "collection `Api`: `bearer` auth was not imported"), 1u) << w;
}
