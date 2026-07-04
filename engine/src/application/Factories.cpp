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

#include <fstream>
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
    // HTTPie export ({"meta":{"format":"httpie",...}}).
    if (contains(head, "\"httpie\"")) {
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
