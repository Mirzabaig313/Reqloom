// Factories — concrete-implementation factory entry points.
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/Hook.h>

#include "ImportFromApidog.h"
#include "ImportFromBruno.h"
#include "ImportFromHoppscotch.h"
#include "ImportFromHttpFile.h"
#include "ImportFromHttpie.h"
#include "ImportFromInsomnia.h"
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
#include "../infrastructure/secrets/KeychainSecretStore.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"
#include "../infrastructure/storage/SqliteHistoryStore.h"
#include "../infrastructure/typings/StaticHookTypingsEmitter.h"

#include <nlohmann/json.hpp>

#include <cstdint>
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

std::unique_ptr<HookRunner> makeQuickJsHookRunner() {
    return std::make_unique<QuickJsHookRunner>();
}

std::unique_ptr<LlmClient> makeHttpLlmClient(HttpClient& transport) {
    return std::make_unique<HttpLlmClient>(transport);
}

ExecutionEngine::Dependencies makeDefaultDependencies() {
    return ExecutionEngine::Dependencies{
        makeCurlHttpClient(),
        makeYamlSchemaParser(),
        makeSqliteHistoryStore(),
        makeKeychainSecretStore(),
        makeQuickJsHookRunner(),
    };
}

std::expected<Project, ReqloomError> parseProject(const std::filesystem::path& reqloomYaml) {
    YamlSchemaParser parser;
    return parser.parse(reqloomYaml);
}

std::expected<void, ReqloomError> validateProject(const Project& project) {
    return DependencyResolver{}.validate(project);
}

std::vector<std::string> collectSecretReferences(const Project& project) {
    return DependencyResolver::collectSecretReferences(project);
}

std::expected<std::filesystem::path, ReqloomError> writeProject(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite) {
    YamlSchemaWriter writer;
    return writer.write(targetDir, project, overwrite);
}

std::expected<std::filesystem::path, ReqloomError> emitHookTypings(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite) {
    StaticHookTypingsEmitter emitter;
    return emitter.emit(targetDir, project, overwrite);
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromOpenApi(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot) {
    ImportFromOpenApi const importer;
    auto inner = importer.run(spec, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromPostman(
    const std::filesystem::path& collection, const std::filesystem::path& projectRoot) {
    ImportFromPostman const importer;
    auto inner = importer.run(collection, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromInsomnia(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    ImportFromInsomnia const importer;
    auto inner = importer.run(exportFile, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromThunderClient(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    ImportFromThunderClient const importer;
    auto inner = importer.run(exportFile, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHoppscotch(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    ImportFromHoppscotch const importer;
    auto inner = importer.run(exportFile, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpFile(
    const std::filesystem::path& file, const std::filesystem::path& projectRoot) {
    ImportFromHttpFile const importer;
    auto inner = importer.run(file, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromBruno(
    const std::filesystem::path& input, const std::filesystem::path& projectRoot) {
    ImportFromBruno const importer;
    auto inner = importer.run(input, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpie(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    ImportFromHttpie const importer;
    auto inner = importer.run(exportFile, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

std::expected<OpenApiImportOutcome, ReqloomError> importFromApidog(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) {
    ImportFromApidog const importer;
    auto inner = importer.run(exportFile, projectRoot);
    if (!inner) {
        return std::unexpected(inner.error());
    }
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

namespace {

/// Read up to 8 KiB of a file's head for content sniffing. Empty on error —
/// the caller then falls back to the default (OpenAPI) importer.
[[nodiscard]] std::string readHead(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string head(8192, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return head;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/// Read a whole file for the structural-detection fallback, bounded to 32 MiB
/// (the largest importer size cap). Empty on error or oversize.
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

/// True only when the head has an HTTPie meta marker: a `"format"` key whose
/// string value is `httpie` (whitespace-tolerant). A bare `contains("httpie")`
/// false-positives on any file that merely mentions httpie (a description, a
/// URL, or HTTPie's own JSON *schema* file), misrouting it to the HTTPie
/// importer where it hard-fails instead of falling through.
[[nodiscard]] bool metaFormatIsHttpie(const std::string& head) {
    constexpr std::string_view key = "\"format\"";
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    std::size_t pos = 0;
    while ((pos = head.find(key, pos)) != std::string::npos) {
        std::size_t i = pos + key.size();
        while (i < head.size() && isSpace(head[i])) {
            ++i;
        }
        if (i < head.size() && head[i] == ':') {
            ++i;
            while (i < head.size() && isSpace(head[i])) {
                ++i;
            }
            if (head.compare(i, 8, "\"httpie\"") == 0) {
                return true;
            }
        }
        pos += key.size();
    }
    return false;
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

/// Pick an importer from a parsed JSON document by its actual structure (not
/// substrings anywhere), so a supported collection is detected even when its
/// marker sits past the fast head-sniff window. Returns nullptr for
/// OpenAPI/Swagger (or anything unrecognised) so the caller falls through.
[[nodiscard]] ImportFn detectJsonImporter(const nlohmann::json& d) {
    const bool obj = d.is_object();
    if (obj && d.contains("meta") && d["meta"].is_object() &&
        d["meta"].value("format", std::string{}) == "httpie") {
        return &importFromHttpie;
    }
    if (obj && d.contains("apidogProject")) {
        return &importFromApidog;
    }
    if (obj && d.contains("info") && d.contains("item") && d["item"].is_array()) {
        return &importFromPostman;
    }
    if (obj && d.contains("__export_format") && d.contains("resources") &&
        d["resources"].is_array()) {
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

}  // namespace

std::expected<OpenApiImportOutcome, ReqloomError> importAny(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot) {
    // `.http` / `.rest` are text (not JSON), so route them by extension before
    // the content sniffs.
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

    const std::string head = readHead(spec);
    // HTTPie export ({"meta":{"format":"httpie",...}}). Match the exact
    // meta.format marker, not a bare "httpie" mention.
    if (metaFormatIsHttpie(head)) {
        return importFromHttpie(spec, projectRoot);
    }
    // Apidog native export.
    if (contains(head, "apidogProject")) {
        return importFromApidog(spec, projectRoot);
    }
    // Postman collection export.
    if (contains(head, "schema.getpostman.com") || contains(head, "_postman_id")) {
        return importFromPostman(spec, projectRoot);
    }
    // Insomnia v4 export.
    if (contains(head, "__export_format") &&
        (contains(head, "insomnia") || contains(head, "\"resources\""))) {
        return importFromInsomnia(spec, projectRoot);
    }
    // Thunder Client (VS Code) collection export.
    if (contains(head, "containerId") || contains(head, "colName")) {
        return importFromThunderClient(spec, projectRoot);
    }
    // Hoppscotch collection export (requests carry an `endpoint` field).
    if (contains(head, "\"endpoint\"")) {
        return importFromHoppscotch(spec, projectRoot);
    }
    // The fast head-sniff missed. A supported JSON collection whose marker sat
    // past the sniff window still gets detected here by parsing the file and
    // dispatching on its actual top-level structure.
    if (const std::string full = readAll(spec); !full.empty()) {
        nlohmann::json doc;
        bool parsed = false;
        try {
            doc = nlohmann::json::parse(full);
            parsed = true;
        } catch (const nlohmann::json::parse_error&) {
            parsed = false;  // not JSON (e.g. a YAML OpenAPI spec) → fall through
        }
        if (parsed) {
            if (const ImportFn fn = detectJsonImporter(doc); fn != nullptr) {
                return fn(spec, projectRoot);
            }
            // A JSON Schema document describes a format; it isn't an export.
            // Say so plainly rather than falling through to a cryptic
            // "not OpenAPI" error (users mistake schema files for exports).
            if (doc.is_object() && doc.contains("$schema") && doc["$schema"].is_string() &&
                doc["$schema"].get<std::string>().find("json-schema.org") != std::string::npos) {
                return std::unexpected(ReqloomError{
                    ErrorCode::SchemaInvalid,
                    ErrorClass::Schema,
                    "import: this is a JSON Schema document (it defines a format), not an API "
                    "export. Export your collection from the tool (Postman, HTTPie, Insomnia, "
                    "etc.) and import that file instead."});
            }
        }
    }
    // Default: OpenAPI (its own error surfaces for unrecognised input).
    return importFromOpenApi(spec, projectRoot);
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
