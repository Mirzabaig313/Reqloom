// Factory functions for engine infrastructure.
//
// The only way CLI / desktop / IPC consumers should construct concrete
// dependencies. Internal headers under `engine/src/` stay private to
// the engine library.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace chainapi::engine {

[[nodiscard]] std::unique_ptr<HttpClient> makeCurlHttpClient();
[[nodiscard]] std::unique_ptr<SchemaParser> makeYamlSchemaParser();
[[nodiscard]] std::unique_ptr<HistoryStore> makeSqliteHistoryStore();
[[nodiscard]] std::unique_ptr<SecretStore> makeKeychainSecretStore();
[[nodiscard]] std::unique_ptr<HookRunner> makeQuickJsHookRunner();

/// Whether the secret store returned by `makeKeychainSecretStore()` is
/// backed by a real OS keychain. False when the engine was built without
/// QtKeychain — in that case the store is a no-op (reads find nothing,
/// writes are dropped), and a UI should warn rather than imply persistence.
[[nodiscard]] bool keychainBackendAvailable() noexcept;

class LlmClient;

/// HTTP-backed LLM client. The transport reference is borrowed — it
/// must outlive the returned LlmClient. Production wiring shares the
/// same CurlHttpClient between the runtime engine and the importer.
[[nodiscard]] std::unique_ptr<LlmClient> makeHttpLlmClient(HttpClient& transport);

/// Convenience: a fully-wired Dependencies bundle with all default
/// implementations. Tests substitute fakes by constructing Dependencies
/// directly.
[[nodiscard]] ExecutionEngine::Dependencies makeDefaultDependencies();

[[nodiscard]] std::expected<Project, ChainApiError> parseProject(
    const std::filesystem::path& chainapiYaml);

/// Collect the distinct `{{secret.NAME}}` references declared anywhere in a
/// project (operation templates, actor auth config, injected headers, etc.),
/// sorted and de-duplicated. The desktop secret manager uses this to show
/// the user exactly which credentials a project needs in the OS keychain —
/// the engine reads exactly these names at run time and never bulk-dumps the
/// keychain.
[[nodiscard]] std::vector<std::string> collectSecretReferences(const Project& project);

/// Write a Project back to a directory as `chainapi.yaml` plus per-entity
/// sub-files (actors/, resources/, environments/). Round-trips with
/// `parseProject` modulo YAML map ordering. Refuses to overwrite an
/// existing `chainapi.yaml` unless `overwrite` is true.
///
/// Returns the path to the written `chainapi.yaml` on success.
[[nodiscard]] std::expected<std::filesystem::path, ChainApiError> writeProject(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite = false);

/// Write `<targetDir>/chainapi.d.ts` describing the hook sandbox `ctx`
/// surface. Hook authors place this file alongside their `.js` scripts
/// so the editor's TypeScript LSP gives autocomplete on `ctx.request`,
/// `ctx.env`, `ctx.hmac`, etc.
[[nodiscard]] std::expected<std::filesystem::path, ChainApiError> emitHookTypings(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite = false);

/// Outcome of an OpenAPI 3.x import. `project` is ready to feed to
/// `writeProject(...)`. `warnings` is a multi-line string containing one
/// human-readable note per imported operation that needs hand-editing
/// (path parameters that must be linked to upstream extractions, etc.).
struct OpenApiImportOutcome {
    Project project;
    std::string warnings;
};

/// Parse an OpenAPI 3.0.x or 3.1.x document (YAML or JSON) into a Project
/// skeleton. One resource per path stem, one operation per (path, method)
/// pair, all tagged with `Provenance{Source::OpenApiImport}`. Inferred
/// extractions are verified against `responses[2xx].content.application/json.example`
/// when an example is present.
///
/// `projectRoot` defines the containment scope: the spec must resolve to
/// a file under this directory, defending against `--spec /etc/passwd`
/// style invocations and `..` traversal.
[[nodiscard]] std::expected<OpenApiImportOutcome, ChainApiError> importFromOpenApi(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot);

}  // namespace chainapi::engine
