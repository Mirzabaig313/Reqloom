// Factories — concrete-implementation factory entry points.
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/Hook.h>
#include <reqloom/engine/ProjectTrustStore.h>

#include "ImportFromApidog.h"
#include "ImportFromBruno.h"
#include "ImportFromHoppscotch.h"
#include "ImportFromHttpFile.h"
#include "ImportFromHttpie.h"
#include "ImportFromInsomnia.h"
#include "ImportFromJsonHelpers.h"
#include "ImportFromOpenApi.h"
#include "ImportFromPostman.h"
#include "ImportFromThunderClient.h"

#include "../domain/DependencyResolver.h"
#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/hooks/QuickJsHookRunner.h"
#include "../infrastructure/http/CurlHttpClient.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/llm/HttpLlmClient.h"
#include "../infrastructure/llm/LlmClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/schema/SchemaWriter.h"
#include "../infrastructure/schema/YamlSchemaParser.h"
#include "../infrastructure/schema/YamlSchemaWriter.h"
#include "../infrastructure/secrets/FileProjectTrustStore.h"
#include "../infrastructure/secrets/KeychainSecretStore.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"
#include "../infrastructure/storage/SqliteHistoryStore.h"
#include "../infrastructure/typings/StaticHookTypingsEmitter.h"

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace reqloom::engine {

namespace {

/// Convert the public sample request to the engine-internal view used by
/// HookRunner. The two mirror each other field-for-field.
[[nodiscard]] HookRequestView toInternal(const HookSampleRequest& req) {
    return HookRequestView{req.method, req.url, req.headers, req.body};
}

[[nodiscard]] HookResponseView toInternal(const HookSampleResponse& resp) {
    return HookResponseView{resp.status, resp.headers, resp.body};
}

[[nodiscard]] HookSampleRequest fromInternal(const HookRequestView& req) {
    return HookSampleRequest{req.method, req.url, req.headers, req.body};
}

[[nodiscard]] HookSampleResponse fromInternal(const HookResponseView& resp) {
    return HookSampleResponse{resp.status, resp.headers, resp.body};
}

}  // namespace

// Dependencies special members (out-of-line for incomplete-type users)

ExecutionEngine::Dependencies::Dependencies() = default;

ExecutionEngine::Dependencies::Dependencies(std::unique_ptr<HttpClient> httpIn,
                                            std::unique_ptr<SchemaParser> schemaIn,
                                            std::unique_ptr<HistoryStore> historyIn,
                                            std::unique_ptr<SecretStore> secretsIn,
                                            std::unique_ptr<HookRunner> hooksIn)
    : http(std::move(httpIn)),
      schema(std::move(schemaIn)),
      history(std::move(historyIn)),
      secrets(std::move(secretsIn)),
      hooks(std::move(hooksIn)) {}

ExecutionEngine::Dependencies::~Dependencies() = default;
ExecutionEngine::Dependencies::Dependencies(Dependencies&&) noexcept = default;
ExecutionEngine::Dependencies& ExecutionEngine::Dependencies::operator=(Dependencies&&) noexcept =
    default;

// Factories

std::unique_ptr<HttpClient> makeCurlHttpClient() {
    return std::make_unique<CurlHttpClient>();
}

std::unique_ptr<SchemaParser> makeYamlSchemaParser() {
    return std::make_unique<YamlSchemaParser>();
}

std::unique_ptr<HistoryStore> makeSqliteHistoryStore() {
    return std::make_unique<SqliteHistoryStore>();
}

std::unique_ptr<SecretStore> makeKeychainSecretStore() {
    return std::make_unique<KeychainSecretStore>();
}

bool keychainBackendAvailable() noexcept {
    return KeychainSecretStore::backendAvailable();
}

std::expected<std::filesystem::path, ReqloomError> defaultProjectTrustStorePath() {
    return FileProjectTrustStore::defaultPath();
}

std::unique_ptr<ProjectTrustStore> makeFileProjectTrustStore() {
    auto path = FileProjectTrustStore::defaultPath();
    if (!path) {
        return std::make_unique<FileProjectTrustStore>(path.error());
    }
    return std::make_unique<FileProjectTrustStore>(std::move(*path));
}

std::unique_ptr<ProjectTrustStore> makeFileProjectTrustStore(const std::filesystem::path& file) {
    return std::make_unique<FileProjectTrustStore>(file);
}

std::unique_ptr<HookRunner> makeQuickJsHookRunner() {
    return std::make_unique<QuickJsHookRunner>();
}

std::unique_ptr<LlmClient> makeHttpLlmClient(HttpClient& transport) {
    return std::make_unique<HttpLlmClient>(transport);
}

ExecutionEngine::Dependencies makeDefaultDependencies() {
    ExecutionEngine::Dependencies deps{
        makeCurlHttpClient(),
        makeYamlSchemaParser(),
        makeSqliteHistoryStore(),
        makeKeychainSecretStore(),
        makeQuickJsHookRunner(),
    };
    deps.trust = makeFileProjectTrustStore();
    return deps;
}

std::expected<Project, ReqloomError> parseProject(const std::filesystem::path& reqloomYaml) {
    YamlSchemaParser parser;
    auto project = parser.parse(reqloomYaml);
    if (project) {
        std::error_code ec;
        auto root = std::filesystem::absolute(reqloomYaml, ec).parent_path();
        if (!ec) {
            project->rootDir = root.lexically_normal();
        }
    }
    return project;
}

std::expected<void, ReqloomError> validateProject(const Project& project) {
    return DependencyResolver{}.validate(project);
}

std::vector<std::string> collectSecretReferences(const Project& project) {
    return DependencyResolver::collectSecretReferences(project);
}

std::expected<std::filesystem::path, ReqloomError> writeProject(
    const std::filesystem::path& targetDir,
    const Project& project,
    bool overwrite,
    bool pruneStale) {
    YamlSchemaWriter writer;
    return writer.write(targetDir, project, overwrite, pruneStale);
}

std::expected<std::filesystem::path, ReqloomError> emitHookTypings(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite) {
    StaticHookTypingsEmitter emitter;
    return emitter.emit(targetDir, project, overwrite);
}

namespace {

/// Run one importer. Import input is untrusted JSON, so an nlohmann exception an importer
/// lets escape becomes a ReqloomError.
template <typename Importer>
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> runImporter(
    const std::filesystem::path& input, const std::filesystem::path& projectRoot) {
    try {
        Importer const importer;
        auto inner = importer.run(input, projectRoot);
        if (!inner) {
            return std::unexpected(inner.error());
        }
        return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                            ErrorClass::Schema,
                                            std::string{"import: malformed input: "} + e.what()});
    }
}

}  // namespace

std::expected<OpenApiImportOutcome, ReqloomError> importFromOpenApi(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromOpenApi>(spec, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromPostman(
    const std::filesystem::path& collection, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromPostman>(collection, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromInsomnia(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromInsomnia>(exportFile, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromThunderClient(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromThunderClient>(exportFile, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHoppscotch(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromHoppscotch>(exportFile, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpFile(
    const std::filesystem::path& file, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromHttpFile>(file, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromBruno(
    const std::filesystem::path& input, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromBruno>(input, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpie(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromHttpie>(exportFile, projectRoot);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromApidog(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    return runImporter<ImportFromApidog>(exportFile, projectRoot);
}

namespace {

/// Read a whole file for detection, bounded to 32 MiB (the largest importer cap); empty on
/// error or oversize.
[[nodiscard]] std::string readAll(const std::filesystem::path& path) {
    std::error_code ec;
    if (const auto size = std::filesystem::file_size(path, ec);
        ec || size > std::uintmax_t{32} * 1024 * 1024) {
        return {};
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using ImportFn = std::expected<OpenApiImportOutcome, ReqloomError> (*)(
    const std::filesystem::path&, const std::filesystem::path&);

/// True if `node` (or a nested folder) has a `requests[]` entry carrying `key`.
/// Depth-bounded so a pathological tree can't blow the stack.
[[nodiscard]] bool requestHasKey(const nlohmann::json& node, std::string_view key, int depth) {
    if (depth > 32 || !node.is_object()) {
        return false;
    }
    if (const auto it = node.find("requests"); it != node.end() && it->is_array()) {
        for (const auto& r : *it) {
            if (r.is_object() && r.contains(key)) {
                return true;
            }
        }
    }
    if (const auto it = node.find("folders"); it != node.end() && it->is_array()) {
        for (const auto& f : *it) {
            if (requestHasKey(f, key, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

/// `info` with `_postman_id` or a getpostman.com `schema`, or the `info` + `item[]` shape.
[[nodiscard]] bool isPostmanCollection(const nlohmann::json& d) {
    const auto info = d.find("info");
    if (info == d.end() || !info->is_object()) {
        return false;
    }
    return info->contains("_postman_id") ||
           import_json::stringOr(*info, "schema", std::string{}).find("getpostman.com") !=
               std::string::npos ||
           (d.contains("item") && d["item"].is_array());
}

/// Pick an importer by top-level structure, never a substring search, so a value that merely
/// mentions another format's marker cannot misroute the file. OpenAPI/Swagger go first.
[[nodiscard]] ImportFn detectJsonImporter(const nlohmann::json& d) {
    const bool obj = d.is_object();
    if (obj && (d.contains("openapi") || d.contains("swagger"))) {
        return &importFromOpenApi;
    }
    if (obj && d.contains("meta") && d["meta"].is_object() &&
        import_json::stringOr(d["meta"], "format", std::string{}) == "httpie") {
        return &importFromHttpie;
    }
    if (obj && d.contains("apidogProject")) {
        return &importFromApidog;
    }
    if (obj && isPostmanCollection(d)) {
        return &importFromPostman;
    }
    if (obj && d.contains("resources") && d["resources"].is_array() &&
        (d.contains("__export_format") ||
         import_json::stringOr(d, "_type", std::string{}) == "export")) {
        return &importFromInsomnia;
    }
    if (obj && d.contains("colName")) {
        return &importFromThunderClient;
    }
    if (d.is_array() && !d.empty() && d.front().is_object()) {
        const auto& e = d.front();
        if (e.contains("colName") || requestHasKey(e, "containerId", 0)) {
            return &importFromThunderClient;
        }
        if (requestHasKey(e, "endpoint", 0) || e.contains("requests") || e.contains("folders")) {
            return &importFromHoppscotch;
        }
    }
    if (obj && (d.contains("folders") || d.contains("requests"))) {
        if (requestHasKey(d, "containerId", 0)) {
            return &importFromThunderClient;
        }
        return &importFromHoppscotch;
    }
    return nullptr;
}

/// How a non-JSON file parses as YAML, the only other structured input.
enum class YamlShape : std::uint8_t { Unparseable, OpenApi, Other };

[[nodiscard]] YamlShape yamlShape(const std::string& text) {
    try {
        const YAML::Node root = YAML::Load(text);
        if (root.IsMap() && (root["openapi"] || root["swagger"])) {
            return YamlShape::OpenApi;
        }
        return YamlShape::Other;
    } catch (const YAML::Exception&) {
        return YamlShape::Unparseable;
    }
}

[[nodiscard]] ReqloomError unrecognisedFormat(std::string_view why) {
    return ReqloomError{
        ErrorCode::SchemaInvalid,
        ErrorClass::Schema,
        std::format("import: unrecognised input format ({}). Supported formats: OpenAPI 3.x and "
                    "Swagger 2.0 (YAML or JSON); Postman Collection v2.0/v2.1, Insomnia v4, "
                    "Thunder Client, Hoppscotch, HTTPie and Apidog JSON exports; a Bruno "
                    "collection (its directory, bruno.json or a .bru file); REST Client "
                    ".http/.rest files",
                    why)};
}

}  // namespace

std::expected<OpenApiImportOutcome, ReqloomError> importAny(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot) {
    // `.http` / `.rest` are text (not JSON), so route them by extension before
    // the content checks.
    std::string ext = spec.extension().string();
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    if (ext == ".http" || ext == ".rest") {
        return importFromHttpFile(spec, projectRoot);
    }
    // Bruno: a collection directory, its bruno.json, or a .bru file.
    std::error_code dirEc;
    if (ext == ".bru" || spec.filename() == "bruno.json" ||
        std::filesystem::is_directory(spec, dirEc)) {
        return importFromBruno(spec, projectRoot);
    }

    // An unreadable, empty or oversized file goes to the OpenAPI importer, whose checks report
    // it precisely.
    const std::string full = readAll(spec);
    if (full.empty()) {
        return importFromOpenApi(spec, projectRoot);
    }
    nlohmann::json doc;
    bool parsed = false;
    try {
        doc = nlohmann::json::parse(full);
        parsed = true;
    } catch (const nlohmann::json::parse_error&) {
        parsed = false;  // not JSON (e.g. a YAML OpenAPI spec)
    }
    if (parsed) {
        if (const ImportFn fn = detectJsonImporter(doc); fn != nullptr) {
            return fn(spec, projectRoot);
        }
        // A JSON Schema document describes a format; users mistake it for an export.
        if (doc.is_object() && doc.contains("$schema") && doc["$schema"].is_string() &&
            doc["$schema"].get<std::string>().find("json-schema.org") != std::string::npos) {
            return std::unexpected(ReqloomError{
                ErrorCode::SchemaInvalid,
                ErrorClass::Schema,
                "import: this is a JSON Schema document (it defines a format), not an API "
                "export. Export your collection from the tool (Postman, HTTPie, Insomnia, "
                "etc.) and import that file instead."});
        }
        return std::unexpected(unrecognisedFormat("JSON with no known top-level structure"));
    }
    switch (yamlShape(full)) {
        case YamlShape::OpenApi:
        case YamlShape::Unparseable:
            return importFromOpenApi(spec, projectRoot);
        case YamlShape::Other:
            break;
    }
    return std::unexpected(
        unrecognisedFormat("not JSON, and YAML without a top-level `openapi`/`swagger` key"));
}

std::expected<HookDryRunResult, ReqloomError> dryRunHook(const HookDryRunInput& input) {
    if (input.script.empty()) {
        return std::unexpected(
            ReqloomError{ErrorCode::HookFailure, ErrorClass::Hook, "hook script is empty"});
    }
    if (input.phase == HookPhase::PostResponse && !input.response) {
        return std::unexpected(ReqloomError{ErrorCode::HookFailure,
                                            ErrorClass::Hook,
                                            "post_response dry-run requires a sample response"});
    }

    HookContext ctx;
    ctx.request = toInternal(input.request);
    if (input.response) {
        ctx.response = toInternal(*input.response);
    }
    ctx.variables = input.variables;
    ctx.env = input.env;
    ctx.secrets = input.secrets;

    QuickJsHookRunner runner;
    auto outcome = input.phase == HookPhase::PreRequest
                       ? runner.runPreRequest(input.script, std::move(ctx))
                       : runner.runPostResponse(input.script, std::move(ctx));
    if (!outcome) {
        return std::unexpected(outcome.error());
    }

    HookDryRunResult result;
    result.request = fromInternal(outcome->mutatedRequest);
    if (outcome->mutatedResponse) {
        result.response = fromInternal(*outcome->mutatedResponse);
    }
    return result;
}

}  // namespace reqloom::engine
