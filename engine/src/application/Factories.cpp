// Factories — concrete-implementation factory entry points.
#include <chainapi/engine/Factories.h>

#include "ImportFromOpenApi.h"

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

namespace chainapi::engine {

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

std::expected<Project, ChainApiError> parseProject(const std::filesystem::path& chainapiYaml) {
    YamlSchemaParser parser;
    return parser.parse(chainapiYaml);
}

std::expected<std::filesystem::path, ChainApiError> writeProject(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite) {
    YamlSchemaWriter writer;
    return writer.write(targetDir, project, overwrite);
}

std::expected<std::filesystem::path, ChainApiError> emitHookTypings(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite) {
    StaticHookTypingsEmitter emitter;
    return emitter.emit(targetDir, project, overwrite);
}

std::expected<OpenApiImportOutcome, ChainApiError> importFromOpenApi(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot) {
    ImportFromOpenApi importer;
    auto inner = importer.run(spec, projectRoot);
    if (!inner) return std::unexpected(inner.error());
    return OpenApiImportOutcome{std::move(inner->project), std::move(inner->warnings)};
}

}  // namespace chainapi::engine
