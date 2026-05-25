// Factories.cpp — concrete-implementation factory entry points.
//
// Lives in the application layer because it must see all infrastructure
// headers; consumers see only `Factories.h` (which exposes interface types
// only).
//
// Also hosts the out-of-line definitions for ExecutionEngine::Dependencies'
// special members (destructor / move), so consumers can store and move
// Dependencies by value without needing the full HttpClient/SchemaParser/
// etc. definitions.
#include <chainapi/engine/Factories.h>

#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/hooks/QuickJsHookRunner.h"
#include "../infrastructure/http/CurlHttpClient.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/schema/YamlSchemaParser.h"
#include "../infrastructure/secrets/KeychainSecretStore.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"
#include "../infrastructure/storage/SqliteHistoryStore.h"

namespace chainapi::engine {

// ─── Dependencies special members (out-of-line for incomplete-type users) ──

ExecutionEngine::Dependencies::Dependencies() = default;

ExecutionEngine::Dependencies::Dependencies(
    std::unique_ptr<HttpClient> httpIn,
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
ExecutionEngine::Dependencies&
ExecutionEngine::Dependencies::operator=(Dependencies&&) noexcept = default;

// ─── Factories ───────────────────────────────────────────────────────────────

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

ExecutionEngine::Dependencies makeDefaultDependencies() {
    return ExecutionEngine::Dependencies{
        makeCurlHttpClient(),
        makeYamlSchemaParser(),
        makeSqliteHistoryStore(),
        makeKeychainSecretStore(),
        makeQuickJsHookRunner(),
    };
}

std::expected<Project, ChainApiError>
parseProject(const std::filesystem::path& chainapiYaml) {
    YamlSchemaParser parser;
    return parser.parse(chainapiYaml);
}

}  // namespace chainapi::engine
