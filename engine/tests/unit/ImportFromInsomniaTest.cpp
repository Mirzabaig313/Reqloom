// Direct (non-LLM) Insomnia v4 importer tests. Each fails on the parent commit
// (importFromInsomnia did not exist).

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <support/ImportFileRefChecks.h>
#include <support/TempPath.h>

#include <expected>
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

// D-20: a non-boolean `disabled` (null, "true", 1) used to throw
// nlohmann::json::type_error out of the importer. It must import cleanly,
// treating the non-boolean value as the default (not disabled).
TEST(ImportFromInsomnia, non_boolean_disabled_does_not_throw) {
    for (const std::string disabled : {"null", "\"true\"", "1"}) {
        ScratchDir scratch;
        const std::string body =
            R"({"_type":"export","__export_format":4,"resources":[
{"_id":"wrk_1","_type":"workspace","name":"D"},
{"_id":"req_1","_type":"request","parentId":"wrk_1","name":"Ping","method":"POST",
"url":"https://api.example.com/ping",
"headers":[{"name":"X-H","value":"v","disabled":)" +
            disabled + R"(}],
"parameters":[{"name":"q","value":"1","disabled":)" +
            disabled + R"(}],
"body":{"mimeType":"application/x-www-form-urlencoded","params":[{"name":"f","value":"x","disabled":)" +
            disabled + R"(}]}}]})";
        const auto file = scratch.write("d.insomnia.json", body);
        std::expected<ce::OpenApiImportOutcome, ce::ReqloomError> outcome =
            std::unexpected(ce::ReqloomError{});
        ASSERT_NO_THROW(outcome = ce::importFromInsomnia(file, scratch.path())) << disabled;
        ASSERT_TRUE(outcome.has_value()) << disabled << ": " << outcome.error().detail;
        ASSERT_EQ(outcome->project.resources.size(), 1u);
        const auto& ops = outcome->project.resources.begin()->second.operations;
        ASSERT_EQ(ops.size(), 1u);
        const auto& op = ops.begin()->second;
        EXPECT_TRUE(op.headers.contains("X-H")) << disabled;
        EXPECT_TRUE(op.queryParams.contains("q")) << disabled;
    }
}

// File fields carrying paths from the exporting machine keep only their base
// name (with a warning); a text field starting with `@` is escaped as `@@`.
TEST(ImportFromInsomnia, file_field_paths_are_reduced_to_base_names_with_warnings) {
    ScratchDir scratch;
    const auto file = scratch.write("uploads.json", R"JSON(
{
  "_type": "export", "__export_format": 4,
  "resources": [
    { "_id": "wrk_1", "_type": "workspace", "name": "Uploads API" },
    { "_id": "fld_1", "_type": "request_group", "parentId": "wrk_1", "name": "Uploads" },
    {
      "_id": "req_1", "_type": "request", "parentId": "fld_1", "name": "Upload",
      "method": "POST", "url": "https://api.example.com/up",
      "body": { "mimeType": "multipart/form-data", "params": [
        { "name": "unix", "type": "file", "fileName": "/Users/x/.ssh/id_rsa" },
        { "name": "win", "type": "file", "fileName": "C:\\Users\\x\\a.png" },
        { "name": "rel", "type": "file", "fileName": "../../secret.txt" },
        { "name": "bare", "type": "file", "fileName": "photo.png" },
        { "name": "sneaky", "value": "@/etc/passwd" }
      ] }
    }
  ]
}
)JSON");

    auto outcome = ce::importFromInsomnia(file, scratch.path());
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

// Insomnia nested environment data (`{{ _.api.url }}`) used to abort the
// import with RefUndefined; it maps to the flattened key `api_url`. The base
// environment is `default`; each sub-environment becomes its own
// environment with its overrides applied, instead of a last-wins merge.
TEST(ImportFromInsomnia, nested_env_and_sub_environments) {
    ScratchDir scratch;
    const auto file = scratch.write("nested.json", R"JSON(
{ "_type": "export", "__export_format": 4, "resources": [
  { "_id": "wrk_1", "_type": "workspace", "name": "Nested" },
  { "_id": "env_base", "_type": "environment", "parentId": "wrk_1", "name": "Base Environment",
    "data": { "api": { "url": "https://base.example.com" }, "region": "eu" } },
  { "_id": "env_dev", "_type": "environment", "parentId": "env_base", "name": "Dev",
    "data": { "api": { "url": "https://dev.example.com" } } },
  { "_id": "env_prod", "_type": "environment", "parentId": "env_base", "name": "Prod",
    "data": { "region": "us" } },
  { "_id": "req_1", "_type": "request", "parentId": "wrk_1", "name": "Ping", "method": "GET",
    "url": "{{ _.api.url }}/ping", "headers": [ { "name": "X-Region", "value": "{{ _.region }}" } ] }
] }
)JSON");
    auto outcome = ce::importFromInsomnia(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& envs = outcome->project.environments;
    EXPECT_EQ(outcome->project.defaultEnvironment, "default");
    ASSERT_TRUE(envs.contains("default"));
    ASSERT_TRUE(envs.contains("dev"));
    ASSERT_TRUE(envs.contains("prod"));
    EXPECT_EQ(envs.at("default").at("api_url"), "https://base.example.com");
    EXPECT_EQ(envs.at("default").at("region"), "eu");
    EXPECT_EQ(envs.at("dev").at("api_url"), "https://dev.example.com");
    EXPECT_EQ(envs.at("dev").at("region"), "eu");
    EXPECT_EQ(envs.at("prod").at("region"), "us");
    EXPECT_EQ(envs.at("default").at("baseUrl"), "https://base.example.com");
    EXPECT_EQ(envs.at("dev").at("baseUrl"), "https://dev.example.com");
    const auto* op = findOp(outcome->project, "nested.ping");
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->pathTemplate, "/ping");
    EXPECT_EQ(op->headers.at("X-Region"), "{{env.region}}");
}

// `{}` and `{"type":"none"}` are "no auth" (the latter used to warn); a real
// scheme warns once per request, and a folder's auth once per folder.
TEST(ImportFromInsomnia, auth_warnings_only_for_real_schemes) {
    ScratchDir scratch;
    const auto file = scratch.write("auth.json", R"JSON(
{ "_type": "export", "__export_format": 4, "resources": [
  { "_id": "wrk_1", "_type": "workspace", "name": "Auth" },
  { "_id": "fld_1", "_type": "request_group", "parentId": "wrk_1", "name": "Admin",
    "authentication": { "type": "basic" } },
  { "_id": "r1", "_type": "request", "parentId": "fld_1", "name": "Empty", "method": "GET",
    "url": "https://api.example.com/a", "authentication": {} },
  { "_id": "r2", "_type": "request", "parentId": "fld_1", "name": "None", "method": "GET",
    "url": "https://api.example.com/b", "authentication": { "type": "none" } },
  { "_id": "r3", "_type": "request", "parentId": "fld_1", "name": "Bearer", "method": "GET",
    "url": "https://api.example.com/c", "authentication": { "type": "bearer", "token": "t" } }
] }
)JSON");
    auto outcome = ce::importFromInsomnia(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& w = outcome->warnings;
    EXPECT_EQ(countOf(w, "auth was not imported"), 2u) << w;
    EXPECT_EQ(countOf(w, "folder `Admin`: `basic` auth was not imported"), 1u) << w;
    EXPECT_EQ(countOf(w, "operation admin.bearer: `bearer` auth was not imported"), 1u) << w;
}
