// Slice 6c-1 — direct OpenAPI 3.x importer skeleton.
//
// Each test fails on the parent commit (importer was a stub returning
// SchemaInvalid). The verification pass arrives in 6c-3 — these tests
// pin only the structural side: paths → resources/operations, methods
// → op-name heuristics, provenance tagging, warnings for unlinked path
// params.

#include <chainapi/engine/Factories.h>

#include <gtest/gtest.h>

#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        const auto unique = "chainapi-openapi-import-" + std::to_string(::getpid()) + "-" +
                            std::to_string(counter_++);
        path_ = fs::temp_directory_path() / unique;
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
    inline static int counter_{0};
};

}  // namespace

TEST(ImportFromOpenApi, rejects_non_openapi_3_documents) {
    ScratchDir scratch;
    const auto spec = scratch.write("swagger2.yaml", R"YAML(
swagger: "2.0"
info:
  title: Old
paths:
  /things:
    get: { responses: { "200": { description: ok } } }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("OpenAPI 3.x"), std::string::npos);
}

TEST(ImportFromOpenApi, rejects_empty_paths) {
    ScratchDir scratch;
    const auto spec = scratch.write("empty.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Empty }
paths: {}
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_NE(outcome.error().detail.find("paths"), std::string::npos);
}

TEST(ImportFromOpenApi, surfaces_yaml_parse_errors_with_YamlParse_code) {
    ScratchDir scratch;
    // Tab in a place yaml-cpp does not tolerate.
    const auto spec = scratch.write("broken.yaml", "openapi: 3.0.3\ninfo:\n\ttitle: tabbed\n");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, ce::ErrorCode::YamlParse);
}

TEST(ImportFromOpenApi, derives_resource_and_operation_names_from_paths) {
    ScratchDir scratch;
    const auto spec = scratch.write("petstore.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Pets }
servers:
  - url: https://api.example.test
paths:
  /pets:
    get:    { responses: { "200": { description: list } } }
    post:   { responses: { "201": { description: created } } }
  /pets/{petId}:
    get:    { responses: { "200": { description: get  } } }
    put:    { responses: { "200": { description: upd  } } }
    delete: { responses: { "204": { description: del  } } }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& project = outcome->project;

    EXPECT_EQ(project.name, "Pets");
    EXPECT_EQ(project.environments.at("default").at("baseUrl"), "https://api.example.test");

    ASSERT_EQ(project.resources.size(), 1u);
    const auto& pet = project.resources.at(ce::ResourceId{"pet"});
    ASSERT_TRUE(pet.operations.contains("list"));
    ASSERT_TRUE(pet.operations.contains("create"));
    ASSERT_TRUE(pet.operations.contains("get"));
    ASSERT_TRUE(pet.operations.contains("update"));
    ASSERT_TRUE(pet.operations.contains("delete"));

    EXPECT_EQ(pet.operations.at("list").method, ce::HttpMethod::Get);
    EXPECT_EQ(pet.operations.at("create").method, ce::HttpMethod::Post);
    EXPECT_EQ(pet.operations.at("get").method, ce::HttpMethod::Get);
    EXPECT_EQ(pet.operations.at("update").method, ce::HttpMethod::Put);
    EXPECT_EQ(pet.operations.at("delete").method, ce::HttpMethod::Delete);

    EXPECT_EQ(pet.operations.at("list").pathTemplate, "/pets");
    EXPECT_EQ(pet.operations.at("get").pathTemplate, "/pets/{{pet.petId}}");
    EXPECT_EQ(pet.operations.at("create").expectStatus, 201);
    EXPECT_EQ(pet.operations.at("delete").expectStatus, 204);
}

TEST(ImportFromOpenApi, tags_every_imported_op_with_provenance) {
    ScratchDir scratch;
    const auto spec = scratch.write("stamped.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Stamped }
paths:
  /widgets:
    get:
      summary: List widgets
      operationId: listWidgets
      responses:
        "200": { description: ok }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;

    const auto& op = outcome->project.resources.at(ce::ResourceId{"widget"}).operations.at("list");
    ASSERT_TRUE(op.provenance.has_value());
    EXPECT_EQ(op.provenance->source, ce::Provenance::Source::OpenApiImport);
    EXPECT_EQ(op.provenance->verifiedAgainst, ce::Provenance::VerifiedAgainst::None);
    ASSERT_TRUE(op.provenance->importedAt.has_value());
    EXPECT_EQ(op.provenance->evidence.at("summary"), "List widgets");
    EXPECT_EQ(op.provenance->evidence.at("operationId"), "listWidgets");
}

TEST(ImportFromOpenApi, warns_about_unlinked_path_parameters) {
    ScratchDir scratch;
    const auto spec = scratch.write("paramwarn.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Linkable }
paths:
  /orders/{orderId}/items/{itemId}:
    get:
      responses: { "200": { description: ok } }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;

    // One warning per rewritten parameter, naming the variable the user
    // must produce upstream.
    EXPECT_NE(outcome->warnings.find("path parameter `orderId`"), std::string::npos);
    EXPECT_NE(outcome->warnings.find("path parameter `itemId`"), std::string::npos);

    const auto& op = outcome->project.resources.at(ce::ResourceId{"item"}).operations.at("get");
    EXPECT_EQ(op.pathTemplate, "/orders/{{item.orderId}}/items/{{item.itemId}}");
    EXPECT_EQ(op.provenance->evidence.at("path_param.orderId").substr(0, 20),
              "rewritten to {{item.");
}

TEST(ImportFromOpenApi, leaves_paths_without_params_untouched) {
    ScratchDir scratch;
    const auto spec = scratch.write("nounlinked.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Plain }
paths:
  /health:
    get: { responses: { "200": { description: ok } } }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->warnings.empty())
        << "no parameters → no warnings, got: " << outcome->warnings;
    const auto& op = outcome->project.resources.at(ce::ResourceId{"health"}).operations.at("list");
    EXPECT_EQ(op.pathTemplate, "/health");
}

TEST(ImportFromOpenApi, picks_first_2xx_for_expectStatus) {
    ScratchDir scratch;
    const auto spec = scratch.write("statuses.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Statuses }
paths:
  /jobs:
    post:
      responses:
        "400": { description: bad  }
        "202": { description: ack  }
        "201": { description: made }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value());
    const auto& op = outcome->project.resources.at(ce::ResourceId{"job"}).operations.at("create");
    // "201" comes before "202" in the YAML map only by ordering, so we
    // tolerate either — the contract is "first 2xx wins".
    EXPECT_TRUE(op.expectStatus.has_value());
    EXPECT_TRUE(*op.expectStatus == 201 || *op.expectStatus == 202);
}

TEST(ImportFromOpenApi, output_round_trips_through_writeProject_parseProject) {
    ScratchDir specDir;
    const auto spec = specDir.write("rt.yaml", R"YAML(
openapi: "3.0.3"
info: { title: RoundTrip }
servers:
  - url: https://api.rt.test
paths:
  /accounts:
    get:  { responses: { "200": { description: list   } } }
    post: { responses: { "201": { description: create } } }
  /accounts/{id}:
    get: { responses: { "200": { description: get } } }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;

    ScratchDir writeDir;
    auto written = ce::writeProject(writeDir.path(), outcome->project);
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    auto reloaded = ce::parseProject(*written);
    ASSERT_TRUE(reloaded.has_value()) << reloaded.error().detail;

    const auto& acc = reloaded->resources.at(ce::ResourceId{"account"});
    EXPECT_TRUE(acc.operations.contains("list"));
    EXPECT_TRUE(acc.operations.contains("create"));
    EXPECT_TRUE(acc.operations.contains("get"));
}

TEST(ImportFromOpenApi, parses_inline_json_input) {
    ScratchDir scratch;
    // OpenAPI documents are commonly distributed as JSON. yaml-cpp parses
    // JSON as a YAML subset.
    const auto spec = scratch.write("petstore.json", R"JSON(
{
  "openapi": "3.0.3",
  "info": { "title": "JsonSpec" },
  "paths": {
    "/things": {
      "get": { "responses": { "200": { "description": "ok" } } }
    }
  }
}
)JSON");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_TRUE(outcome->project.resources.contains(ce::ResourceId{"thing"}));
}

// ─── Slice 6c-3 — extraction inference + verification ───────────────────────

TEST(ImportFromOpenApi, infers_extractions_from_top_level_scalar_schema) {
    ScratchDir scratch;
    const auto spec = scratch.write("inferred.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Inferred }
paths:
  /widgets/{id}:
    get:
      responses:
        "200":
          content:
            application/json:
              schema:
                type: object
                properties:
                  id:        { type: string }
                  name:      { type: string }
                  count:     { type: integer }
                  available: { type: boolean }
                  nested:    { type: object }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& op = outcome->project.resources.at(ce::ResourceId{"widget"}).operations.at("get");

    ASSERT_EQ(op.extractions.size(), 4u) << "scalar fields only";
    std::map<std::string, std::string> got;
    for (const auto& e : op.extractions) got[e.variableName] = e.sourcePath;
    EXPECT_EQ(got.at("id"), "$.id");
    EXPECT_EQ(got.at("name"), "$.name");
    EXPECT_EQ(got.at("count"), "$.count");
    EXPECT_EQ(got.at("available"), "$.available");
    EXPECT_FALSE(got.contains("nested"));
}

TEST(ImportFromOpenApi, infers_extractions_under_data_wrapper) {
    ScratchDir scratch;
    const auto spec = scratch.write("wrapped.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Wrapped }
paths:
  /accounts/{id}:
    get:
      responses:
        "200":
          content:
            application/json:
              schema:
                type: object
                properties:
                  data:
                    type: object
                    properties:
                      account_id: { type: string }
                      balance:    { type: number }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& op = outcome->project.resources.at(ce::ResourceId{"account"}).operations.at("get");
    ASSERT_EQ(op.extractions.size(), 2u);
    std::map<std::string, std::string> got;
    for (const auto& e : op.extractions) got[e.variableName] = e.sourcePath;
    EXPECT_EQ(got.at("account_id"), "$.data.account_id");
    EXPECT_EQ(got.at("balance"), "$.data.balance");
}

TEST(ImportFromOpenApi, verifies_extractions_against_response_example) {
    ScratchDir scratch;
    const auto spec = scratch.write("verified.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Verified }
paths:
  /pets/{id}:
    get:
      responses:
        "200":
          content:
            application/json:
              schema:
                type: object
                properties:
                  id:   { type: string }
                  name: { type: string }
              example:
                id: pet-123
                name: Rex
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    const auto& op = outcome->project.resources.at(ce::ResourceId{"pet"}).operations.at("get");

    ASSERT_TRUE(op.provenance.has_value());
    EXPECT_EQ(op.provenance->verifiedAgainst, ce::Provenance::VerifiedAgainst::OpenApiExample);
    EXPECT_NE(op.provenance->evidence.at("extract.id").substr(0, 9), "no_match:");
    EXPECT_EQ(op.provenance->evidence.at("extract.id").substr(0, 9), "verified:");
    EXPECT_EQ(op.provenance->evidence.at("extract.name").substr(0, 9), "verified:");
}

TEST(ImportFromOpenApi, warns_when_example_disagrees_with_schema) {
    ScratchDir scratch;
    // Example is missing the `name` scalar that the schema promises.
    const auto spec = scratch.write("mismatch.yaml", R"YAML(
openapi: "3.0.3"
info: { title: Mismatched }
paths:
  /pets/{id}:
    get:
      responses:
        "200":
          content:
            application/json:
              schema:
                type: object
                properties:
                  id:   { type: string }
                  name: { type: string }
              example:
                id: pet-123
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_NE(outcome->warnings.find("did not match"), std::string::npos);

    const auto& op = outcome->project.resources.at(ce::ResourceId{"pet"}).operations.at("get");
    EXPECT_EQ(op.provenance->evidence.at("extract.name").substr(0, 9), "no_match:");
}

TEST(ImportFromOpenApi, warns_when_extractions_inferred_but_no_example) {
    ScratchDir scratch;
    const auto spec = scratch.write("nosample.yaml", R"YAML(
openapi: "3.0.3"
info: { title: NoSample }
paths:
  /pets/{id}:
    get:
      responses:
        "200":
          content:
            application/json:
              schema:
                type: object
                properties:
                  id:   { type: string }
                  name: { type: string }
)YAML");

    auto outcome = ce::importFromOpenApi(spec);
    ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
    EXPECT_NE(outcome->warnings.find("no response example"), std::string::npos)
        << "actual warnings: " << outcome->warnings;

    const auto& op = outcome->project.resources.at(ce::ResourceId{"pet"}).operations.at("get");
    EXPECT_EQ(op.provenance->verifiedAgainst, ce::Provenance::VerifiedAgainst::None);
}
