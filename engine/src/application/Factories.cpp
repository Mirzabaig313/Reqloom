// Factories — concrete-implementation factory entry points.
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/Hook.h>

#include "ImportFromOpenApi.h"

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
