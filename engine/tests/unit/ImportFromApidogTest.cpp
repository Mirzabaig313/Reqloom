// Direct (non-LLM) Apidog native export importer tests. Each fails on the
// parent commit (importFromApidog did not exist).

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
        path_ = reqloom::tests::uniqueTempPath("reqloom-apidog-import");
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

// Mirrors the real Apidog shape: a synthetic "Root" folder wrapping an
// "Authentication" folder with a POST endpoint (JSON body example + header),
// plus a GET endpoint with a query parameter.
constexpr const char* kExport = R"JSON(
{
  "apidogProject": "1.0.0",
  "$schema": { "app": "apidog", "type": "project", "version": "1.2.0" },
  "info": { "name": "RHP School API", "description": "" },
  "apiCollection": [
    {
      "name": "Root", "id": 1, "parentId": 0,
      "items": [
        {
          "name": "Authentication", "id": 2, "parentId": 0,
          "items": [
            {
              "name": "Login",
              "api": {
                "id": "10", "method": "post", "path": "/auth/login",
                "parameters": {
                  "path": [], "query": [], "cookie": [],
                  "header": [ { "name": "X-Api-Key", "example": "{{apiKey}}", "enable": true } ]
                },
                "requestBody": {
                  "type": "application/json",
                  "parameters": [],
                  "examples": [ { "value": "{\"email\":\"a@b.com\"}", "mediaType": "application/json" } ]
                },
                "responses": [ { "code": 200, "contentType": "json" } ]
              }
            },
            {
              "name": "List Sessions",
              "api": {
                "id": "11", "method": "get", "path": "/auth/sessions",
                "parameters": {
                  "path": [], "cookie": [], "header": [],
                  "query": [ { "name": "status", "example": "active", "enable": true } ]
                },
                "responses": [ { "code": 200 } ]
              }
            }
          ]
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

TEST(ImportFromApidog, rejects_non_apidog_json) {
    ScratchDir scratch;
    const auto file = scratch.write("nope.json", R"({"openapi":"3.0.0","paths":{}})");
    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("Apidog"), std::string::npos);
}

TEST(ImportFromApidog, unwraps_root_and_folder_becomes_resource) {
    ScratchDir scratch;
    const auto file = scratch.write("rhp.apidog.json", kExport);

    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    EXPECT_EQ(project.name, "RHP School API");
    // "Root" is unwrapped; "Authentication" is the resource.
    ASSERT_TRUE(project.resources.contains(ce::ResourceId{"authentication"}));
    EXPECT_EQ(project.resources.at(ce::ResourceId{"authentication"}).operations.size(), 2u);

    const auto* login = findOp(project, "authentication.login");
    ASSERT_NE(login, nullptr);
    ASSERT_TRUE(login->provenance.has_value());
    EXPECT_EQ(login->provenance->source, ce::Provenance::Source::ApidogImport);
    EXPECT_EQ(login->method, ce::HttpMethod::Post);
    EXPECT_EQ(login->pathTemplate, "/auth/login");
    ASSERT_TRUE(login->bodyTemplate.has_value());
    EXPECT_NE(login->bodyTemplate->find("a@b.com"), std::string::npos);
    ASSERT_TRUE(login->headers.contains("X-Api-Key"));
    EXPECT_EQ(login->headers.at("X-Api-Key"), "{{env.apiKey}}");
    EXPECT_EQ(login->expectStatus, 200);
}

TEST(ImportFromApidog, keeps_query_params) {
    ScratchDir scratch;
    const auto file = scratch.write("rhp2.json", kExport);

    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;

    const auto* list = findOp(outcome->project, "authentication.list_sessions");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->method, ce::HttpMethod::Get);
    ASSERT_TRUE(list->queryParams.contains("status"));
    EXPECT_EQ(list->queryParams.at("status"), "active");
}

TEST(ImportFromApidog, importAny_routes_apidog_export) {
    ScratchDir scratch;
    const auto file = scratch.write("rhp3.json", kExport);
    auto outcome = ce::importAny(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome->project.resources.contains(ce::ResourceId{"authentication"}));
}

TEST(ImportFromApidog, tolerates_non_object_parameter_elements) {
    ScratchDir scratch;
    // A crafted export with non-object entries in the parameter arrays must not
    // crash (json::value throws on non-objects) — it should import cleanly.
    const auto file = scratch.write("weird.apidog.json", R"JSON(
{
  "apidogProject": "1.0.0",
  "info": { "name": "Weird" },
  "apiCollection": [
    { "name": "Root", "id": 1, "parentId": 0, "items": [
      { "name": "Ping", "api": {
        "method": "get", "path": "/ping",
        "parameters": { "header": [ 42, "oops" ], "query": [ null ] },
        "requestBody": { "type": "multipart/form-data", "parameters": [ 7 ] }
      } }
    ] }
  ]
}
)JSON");

    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* ping = findOp(outcome->project, "weird.ping");
    ASSERT_NE(ping, nullptr);
    EXPECT_TRUE(ping->headers.empty());
    EXPECT_TRUE(ping->queryParams.empty());
}

TEST(ImportFromApidog, rejects_traversal_outside_project_root) {
    ScratchDir scratch;
    const auto outside = scratch.path().parent_path() / "reqloom-apidog-escape.json";
    {
        std::ofstream out{outside};
        out << kExport;
    }
    auto outcome = ce::importFromApidog(outside, scratch.path());
    std::error_code ec;
    fs::remove(outside, ec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("project root"), std::string::npos);
}

// Apidog file parameters carry no path (always a bare `@` slot); a text
// parameter whose example starts with `@` is escaped as `@@` so it stays text.
TEST(ImportFromApidog, text_field_starting_with_at_is_escaped_as_text) {
    ScratchDir scratch;
    const auto file = scratch.write("uploads.apidog.json", R"JSON(
{
  "apidogProject": "1.0.0",
  "info": { "name": "Uploads API" },
  "apiCollection": [
    {
      "name": "Root", "id": 1, "parentId": 0,
      "items": [
        {
          "name": "Uploads", "id": 2, "parentId": 0,
          "items": [
            {
              "name": "Upload",
              "api": {
                "id": "10", "method": "post", "path": "/up",
                "requestBody": { "type": "multipart/form-data", "parameters": [
                  { "name": "avatar", "type": "file", "enable": true },
                  { "name": "sneaky", "type": "string", "example": "@/etc/passwd", "enable": true },
                  { "name": "plain", "type": "string", "example": "hello", "enable": true }
                ] }
              }
            }
          ]
        }
      ]
    }
  ]
}
)JSON");

    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* op = findOp(outcome->project, "uploads.upload");
    ASSERT_NE(op, nullptr);
    ASSERT_TRUE(op->bodyForm.has_value());
    EXPECT_EQ(op->bodyForm->at("avatar"), "@");
    EXPECT_EQ(op->bodyForm->at("sneaky"), "@@/etc/passwd");
    EXPECT_EQ(op->bodyForm->at("plain"), "hello");
    EXPECT_EQ(outcome->warnings.find("`sneaky`"), std::string::npos) << outcome->warnings;
    EXPECT_EQ(outcome->warnings.find("`plain`"), std::string::npos) << outcome->warnings;
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

// An Apidog `{id}` path parameter becomes the shared `{{resource.id}}`
// reference; endpoint auth gives one warning and `"auth": {}` none.
TEST(ImportFromApidog, path_parameter_and_auth_are_handled_like_other_importers) {
    ScratchDir scratch;
    const auto file = scratch.write("users.apidog.json", R"JSON(
{
  "apidogProject": "1.0.0",
  "info": { "name": "Users API" },
  "apiCollection": [ { "name": "Root", "items": [ { "name": "Users", "items": [
    { "name": "Get User", "api": { "method": "get", "path": "/users/{id}", "auth": {},
      "parameters": { "path": [ { "name": "id", "example": "42" } ], "query": [], "header": [] } } },
    { "name": "Delete User", "api": { "method": "delete", "path": "/users/{id}",
      "auth": { "type": "bearer" }, "parameters": { "path": [], "query": [], "header": [] } } }
  ] } ] } ]
}
)JSON");
    auto outcome = ce::importFromApidog(file, scratch.path());
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto* get = findOp(outcome->project, "users.get_user");
    ASSERT_NE(get, nullptr);
    EXPECT_EQ(get->pathTemplate, "/users/{{users.id}}");
    const auto& w = outcome->warnings;
    EXPECT_NE(w.find("path parameter `id` resolves to {{users.id}}"), std::string::npos) << w;
    EXPECT_NE(w.find("`42`"), std::string::npos) << w;
    EXPECT_EQ(countOf(w, "auth was not imported"), 1u) << w;
    EXPECT_EQ(countOf(w, "operation users.delete_user: `bearer` auth was not imported"), 1u) << w;
}
