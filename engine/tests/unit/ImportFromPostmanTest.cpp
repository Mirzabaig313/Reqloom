// Direct (non-LLM) Postman Collection v2.1 importer tests. Each fails on the
// parent commit (importFromPostman did not exist).

#include "application/ImportFromFileRefs.h"

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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
    // Only the base name of the exporting machine's path is kept.
    EXPECT_EQ(form.at("avatar"), "@pic.png");
    // A file field with no source path is preserved as an `@` placeholder to be
    // filled before running — not silently dropped.
    EXPECT_EQ(form.at("empty"), "@");

    // An empty file field is the normal Postman case (exports carry no path),
    // so it produces no warning — the editor shows it as a "Choose a file…"
    // slot. The dropped directory of `avatar` is the only warning.
    EXPECT_NE(outcome->warnings.find("`avatar`"), std::string::npos) << outcome->warnings;
    EXPECT_EQ(outcome->warnings.find("`empty`"), std::string::npos) << outcome->warnings;
}

// File fields carrying paths from the exporting machine (Unix absolute,
// Windows absolute, relative `../`) keep only their base name, each with a
// warning; a bare name is untouched and silent. A text field whose value
// starts with `@` would be sent as an upload, so it is warned about and
// reduced the same way.
constexpr const char* kFilePathsCollection = R"JSON(
{
  "info": { "name": "Uploads", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    {
      "name": "Upload",
      "request": {
        "method": "POST",
        "url": { "raw": "https://api.example.com/up", "path": ["up"] },
        "body": {
          "mode": "formdata",
          "formdata": [
            { "key": "unix", "type": "file", "src": "/Users/x/.ssh/id_rsa" },
            { "key": "win", "type": "file", "src": "C:\\Users\\x\\a.png" },
            { "key": "rel", "type": "file", "src": "../../secret.txt" },
            { "key": "bare", "type": "file", "src": "photo.png" },
            { "key": "multi", "type": "file", "src": ["/a/one.png", "/b/two.png"] },
            { "key": "dironly", "type": "file", "src": "/Users/x/" },
            { "key": "sneaky", "type": "text", "value": "@/etc/passwd" }
          ]
        }
      }
    }
  ]
}
)JSON";

TEST(ImportFromPostman, file_field_paths_are_reduced_to_base_names_with_warnings) {
    ScratchDir scratch;
    const auto file = scratch.write("paths.json", kFilePathsCollection);

    auto outcome = ce::importFromPostman(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* op = findOp(outcome->project, "uploads.upload");
    ASSERT_NE(op, nullptr);
    ASSERT_TRUE(op->bodyForm.has_value());
    const auto& form = *op->bodyForm;
    EXPECT_EQ(form.at("unix"), "@id_rsa");
    EXPECT_EQ(form.at("win"), "@a.png");
    EXPECT_EQ(form.at("rel"), "@secret.txt");
    EXPECT_EQ(form.at("bare"), "@photo.png");
    EXPECT_EQ(form.at("multi"), "@one.png");
    EXPECT_EQ(form.at("dironly"), "@");
    EXPECT_EQ(form.at("sneaky"), "@passwd");

    const auto& w = outcome->warnings;
    for (const char* field : {"`unix`", "`win`", "`rel`", "`dironly`", "`sneaky`"}) {
        EXPECT_NE(w.find(field), std::string::npos) << field << '\n' << w;
    }
    EXPECT_EQ(w.find("`bare`"), std::string::npos) << w;
    EXPECT_NE(w.find("operation uploads.upload"), std::string::npos) << w;
    EXPECT_NE(w.find("current working directory"), std::string::npos) << w;
}

// A file name made of a variable is resolved only at run time, after the
// importer's directory stripping, so an imported environment value could put
// a full path back. Such a reference is imported as an empty file slot.
TEST(ImportFileRefs, file_reference_built_from_a_variable_becomes_an_empty_slot) {
    using ce::import_files::fileFieldValue;
    using ce::import_files::textFieldValue;
    const auto identity = [](std::string_view s) {
        return std::string{s};
    };

    std::vector<std::string> warnings;
    EXPECT_EQ(fileFieldValue("{{file}}", "up.load", "doc", warnings, identity), "@");
    ASSERT_EQ(warnings.size(), 1U);
    EXPECT_NE(warnings[0].find("variable"), std::string::npos) << warnings[0];
    EXPECT_NE(warnings[0].find("operation up.load"), std::string::npos) << warnings[0];

    warnings.clear();
    EXPECT_EQ(fileFieldValue("/tmp/pre-{{file}}.png", "up.load", "doc", warnings, identity), "@");
    EXPECT_FALSE(warnings.empty());

    warnings.clear();
    EXPECT_EQ(textFieldValue("@{{file}}", "up.load", "note", warnings, identity), "@");
    EXPECT_FALSE(warnings.empty());

    // A plain name is unaffected.
    warnings.clear();
    EXPECT_EQ(fileFieldValue("photo.png", "up.load", "doc", warnings, identity), "@photo.png");
    EXPECT_TRUE(warnings.empty());
}

TEST(ImportFileRefs, base_name_handles_separators_drives_and_edge_cases) {
    using ce::import_files::fileBaseName;
    struct Case {
        const char* in;
        const char* name;
        bool hadDirectory;
    };
    const Case cases[] = {
        {"photo.png", "photo.png", false},
        {"", "", false},
        {"/Users/x/docs/photo.png", "photo.png", true},
        {"C:\\Users\\x\\a.png", "a.png", true},
        {"C:/Users/x/a.png", "a.png", true},
        {"C:a.png", "a.png", true},
        {"\\\\server\\share\\a.png", "a.png", true},
        {"../../secret.txt", "secret.txt", true},
        {"./a.png", "a.png", true},
        {"/Users/x/", "", true},
        {"C:\\", "", true},
        {"..", "", true},
        {".", "", true},
        {"a/..", "", true},
        {"dir\\mixed/b.png", "b.png", true},
    };
    for (const auto& c : cases) {
        const auto got = fileBaseName(c.in);
        EXPECT_EQ(got.name, c.name) << c.in;
        EXPECT_EQ(got.hadDirectory, c.hadDirectory) << c.in;
    }

    std::vector<std::string> warnings;
    const auto identity = [](std::string_view s) {
        return std::string{s};
    };
    EXPECT_EQ(ce::import_files::fileFieldValue("x.png", "r.op", "f", warnings, identity), "@x.png");
    EXPECT_EQ(ce::import_files::fileFieldValue("", "r.op", "f", warnings, identity), "@");
    EXPECT_EQ(ce::import_files::textFieldValue("plain", "r.op", "t", warnings, identity), "plain");
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(ce::import_files::fileFieldValue("/a/x.png", "r.op", "f", warnings, identity),
              "@x.png");
    EXPECT_EQ(ce::import_files::textFieldValue("@handle", "r.op", "t", warnings, identity),
              "@handle");
    EXPECT_EQ(warnings.size(), 2U);
}

TEST(ImportFromPostman, importAny_detects_collection_when_marker_past_head_window) {
    // A Postman collection whose `schema`/`_postman_id` markers sit AFTER a
    // >8 KiB leading description — the fast head-sniff misses them, but
    // importAny's structural fallback (info + item[]) still routes to Postman.
    ScratchDir scratch;
    const std::string pad(9000, 'x');
    const std::string body = std::string{"{\"info\":{\"description\":\""} + pad +
                             "\",\"_postman_id\":\"abc-123\",\"name\":\"Deep\","
                             "\"schema\":\"https://schema.getpostman.com/json/collection/"
                             "v2.1.0/collection.json\"},"
                             "\"item\":[{\"name\":\"Ping\",\"request\":{\"method\":\"GET\","
                             "\"url\":{\"raw\":\"https://api.example.com/ping\","
                             "\"path\":[\"ping\"]}}}]}";
    const auto file = scratch.write("deep.json", body);

    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->project.name, "Deep");
    // Routed to the Postman importer (not the OpenAPI default) despite the
    // marker being past the head-sniff window.
    ASSERT_FALSE(outcome->project.resources.empty());
}

// D-20: a non-boolean `disabled` (null, "true", 1) used to throw
// nlohmann::json::type_error out of the importer. It must import cleanly,
// treating the non-boolean value as the default (not disabled).
TEST(ImportFromPostman, non_boolean_disabled_does_not_throw) {
    for (const std::string disabled : {"null", "\"true\"", "1"}) {
        ScratchDir scratch;
        const std::string body =
            R"({"info":{"name":"D","schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json"},
"item":[{"name":"Ping","request":{"method":"POST",
"header":[{"key":"X-H","value":"v","disabled":)" +
            disabled + R"(}],
"url":{"raw":"https://api.example.com/ping?q=1","path":["ping"],
"query":[{"key":"q","value":"1","disabled":)" +
            disabled + R"(}]},
"body":{"mode":"urlencoded","urlencoded":[{"key":"f","value":"x","disabled":)" +
            disabled + R"(}]}}}]})";
        const auto file = scratch.write("d.json", body);
        std::expected<ce::OpenApiImportOutcome, ce::ReqloomError> outcome =
            std::unexpected(ce::ReqloomError{});
        ASSERT_NO_THROW(outcome = ce::importFromPostman(file, scratch.path())) << disabled;
        ASSERT_TRUE(outcome.has_value()) << disabled << ": " << outcome.error().detail;
        ASSERT_EQ(outcome->project.resources.size(), 1u);
        const auto& ops = outcome->project.resources.begin()->second.operations;
        ASSERT_EQ(ops.size(), 1u);
        const auto& op = ops.begin()->second;
        EXPECT_TRUE(op.headers.contains("X-H")) << disabled;
        EXPECT_TRUE(op.queryParams.contains("q")) << disabled;
    }
}

// D-20: the import sniffer's typed `meta.format` lookup threw on a non-string
// value; importAny must still route the document (here to Postman).
TEST(ImportFromPostman, importAny_tolerates_non_string_meta_format) {
    ScratchDir scratch;
    const auto file = scratch.write("m.json",
                                    R"({"meta":{"format":1},"info":{"name":"Meta"},
"item":[{"name":"Ping","request":{"method":"GET","url":"https://api.example.com/ping"}}]})");
    std::expected<ce::OpenApiImportOutcome, ce::ReqloomError> outcome =
        std::unexpected(ce::ReqloomError{});
    ASSERT_NO_THROW(outcome = ce::importAny(file, scratch.path()));
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_EQ(outcome->project.name, "Meta");
}
