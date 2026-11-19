// Factory functions for engine infrastructure.
// The only way CLI / desktop / IPC consumers should construct concrete dependencies.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/ImportCredentials.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::engine {

[[nodiscard]] std::unique_ptr<HttpClient> makeCurlHttpClient();
[[nodiscard]] std::unique_ptr<SchemaParser> makeYamlSchemaParser();
[[nodiscard]] std::unique_ptr<HistoryStore> makeSqliteHistoryStore();
[[nodiscard]] std::unique_ptr<SecretStore> makeKeychainSecretStore();
[[nodiscard]] std::unique_ptr<HookRunner> makeQuickJsHookRunner();

/// Whether `makeKeychainSecretStore()` is backed by a real OS keychain. False without QtKeychain:
/// reads find nothing and writes fail with `SecretAccessFailed`, so a UI should warn.
[[nodiscard]] bool keychainBackendAvailable() noexcept;

/// Where the per-user project trust file lives: `projects.json` in the application config
/// directory of the current user account. No environment variable moves it (the home directory
/// comes from the account database on macOS and Linux; Windows still reads `APPDATA`).
[[nodiscard]] std::expected<std::filesystem::path, ReqloomError> defaultProjectTrustStorePath();

/// Test-only seam: put the trust file in `directory` instead (nullopt restores the default). The
/// shipped programs never call it; test executables call it before `main`.
void setConfigDirectoryForTesting(std::optional<std::filesystem::path> directory);
[[nodiscard]] std::optional<std::filesystem::path> configDirectoryForTesting();

/// The run-history database of the project in `projectRoot`, shared by the desktop app and the
/// CLI: `history/history-<sha1 of the canonical folder>.db` in the desktop app's per-user data
/// directory (`~/Library/Application Support/Reqloom/Reqloom` on macOS), or in the directory
/// `setConfigDirectoryForTesting` names. Creates nothing; `ExecutionEngine::openHistory` does.
[[nodiscard]] std::expected<std::filesystem::path, ReqloomError> defaultHistoryDatabasePath(
    const std::filesystem::path& projectRoot);

/// The file-backed ProjectTrustStore at `defaultProjectTrustStorePath()`. If that path cannot
/// be resolved, every call reports the error.
[[nodiscard]] std::unique_ptr<ProjectTrustStore> makeFileProjectTrustStore();

/// The file-backed ProjectTrustStore at an explicit file path (tests, tools).
[[nodiscard]] std::unique_ptr<ProjectTrustStore> makeFileProjectTrustStore(
    const std::filesystem::path& file);

/// The file-backed HostApprovalStore (approved hosts per project, FR-15.9). It shares the trust
/// file at `defaultProjectTrustStorePath()`, so approvals are bound to the folder that owns each
/// project id. If that path cannot be resolved, every call reports the error.
[[nodiscard]] std::unique_ptr<HostApprovalStore> makeFileHostApprovalStore();

/// The file-backed HostApprovalStore at an explicit trust file path (tests, tools).
[[nodiscard]] std::unique_ptr<HostApprovalStore> makeFileHostApprovalStore(
    const std::filesystem::path& file);

class LlmClient;

/// HTTP-backed LLM client. The transport reference is borrowed and must outlive the client.
[[nodiscard]] std::unique_ptr<LlmClient> makeHttpLlmClient(HttpClient& transport);

/// A fully-wired Dependencies bundle with all default implementations.
[[nodiscard]] ExecutionEngine::Dependencies makeDefaultDependencies();

[[nodiscard]] std::expected<Project, ReqloomError> parseProject(
    const std::filesystem::path& reqloomYaml);

/// Run the structural validation `parseProject` applies (cycles, undefined references) against an
/// already-built Project, so an invalid in-memory edit is rejected before `writeProject`.
[[nodiscard]] std::expected<void, ReqloomError> validateProject(const Project& project);

/// The distinct `{{secret.NAME}}` references declared anywhere in a project, sorted; the secret
/// manager shows these as the credentials the project needs, and the engine reads exactly these
/// names.
[[nodiscard]] std::vector<std::string> collectSecretReferences(const Project& project);

/// Write a Project to a directory as `reqloom.yaml` plus per-entity sub-files (actors/, resources/,
/// environments/); round-trips with `parseProject` modulo map ordering. Refuses to overwrite an
/// existing `reqloom.yaml` unless `overwrite`.
///
/// With `overwrite` and `pruneStale` (the in-place save default), stale `*.yaml` files in those
/// folders are deleted; `pruneStale = false` replaces only the files being written.
///
/// Actor ids, resource ids and environment names must be safe file stems (non-empty, no leading
/// dot, no `/`, `\`, `:` or NUL); otherwise an error names the id and nothing is written.
///
/// `loadedFiles` (normally `&project.loadedFiles`) guards against other writers: pruning deletes
/// only files in it, writing a file on disk but not in it is an error, as is replacing or pruning a
/// file changed since load. On success it is updated to the files now on disk. Null keeps the
/// unguarded behaviour.
///
/// Returns the path to the written `reqloom.yaml`.
[[nodiscard]] std::expected<std::filesystem::path, ReqloomError> writeProject(
    const std::filesystem::path& targetDir,
    const Project& project,
    bool overwrite = false,
    bool pruneStale = true,
    std::map<std::filesystem::path, std::size_t>* loadedFiles = nullptr);

/// Write `<targetDir>/reqloom.d.ts` describing the hook sandbox `ctx` surface, for editor
/// autocomplete.
[[nodiscard]] std::expected<std::filesystem::path, ReqloomError> emitHookTypings(
    const std::filesystem::path& targetDir, const Project& project, bool overwrite = false);

/// Outcome of an import. `project` is ready for `writeProject`; `warnings` holds one note per
/// operation that needs hand-editing.
///
/// `credentials` lists credentials found (see `detectImportedCredentials`); `project` still holds
/// their plain values until `writeImportedProject` moves them.
struct OpenApiImportOutcome {
    Project project;
    std::string warnings;
    std::vector<ImportedCredential> credentials;
};

/// Parse an OpenAPI 3.0.x or 3.1.x document (YAML or JSON) into a Project skeleton: one resource
/// per path stem, one operation per (path, method), tagged `Provenance{Source::OpenApiImport}`.
/// Inferred extractions are verified against a 2xx `application/json` example when present.
///
/// `projectRoot` is the containment scope: the spec must resolve to a file under it (blocks `..`
/// traversal).
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromOpenApi(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot);

/// Parse a Postman Collection v2.1 export (JSON) into a Project skeleton. Folders become resources,
/// requests operations, collection variables seed the `default` environment, and a request URL's
/// scheme+host its `baseUrl`. Reuses `OpenApiImportOutcome`; `provenance.source` is
/// `PostmanImport`.
///
/// `projectRoot` is the containment scope: the collection must resolve to a file under it.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromPostman(
    const std::filesystem::path& collection, const std::filesystem::path& projectRoot);

/// Parse an Insomnia v4 JSON export into a Project skeleton (request groups become resources,
/// requests operations, environment `data` seeds `default`). Reuses `OpenApiImportOutcome`;
/// `provenance.source` is `InsomniaImport`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromInsomnia(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot);

/// Parse a Thunder Client (VS Code) collection export (JSON) into a Project (folders become
/// resources, requests operations). Reuses `OpenApiImportOutcome`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromThunderClient(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot);

/// Parse a Hoppscotch collection export (JSON) into a Project; `<<var>>` references become
/// `{{env.var}}`. Reuses `OpenApiImportOutcome`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromHoppscotch(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot);

/// Parse a `.http` / `.rest` file (VS Code REST Client / JetBrains) into a Project: requests split
/// by `###`, `@name = value` seeds `default`, `{{var}}` becomes `{{env.var}}`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpFile(
    const std::filesystem::path& file, const std::filesystem::path& projectRoot);

/// Parse a Bruno collection into a Project. `input` may be the collection directory, its
/// `bruno.json`, or a `.bru` file inside it; `environments/*.bru` seed environments. Reuses
/// `OpenApiImportOutcome`; `provenance.source` is `BrunoImport`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromBruno(
    const std::filesystem::path& input, const std::filesystem::path& projectRoot);

/// Parse an HTTPie client export (JSON) into a Project; `entry` may be a workspace, collection,
/// request or environment. Reuses `OpenApiImportOutcome`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromHttpie(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot);

/// Parse an Apidog native export (JSON) into a Project: the `apiCollection` folder tree becomes
/// resources (the synthetic "Root" is unwrapped) and each endpoint's `api` an operation. Reuses
/// `OpenApiImportOutcome`.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importFromApidog(
    const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot);

/// Detect the format of `spec` (OpenAPI or an API-client export) by content sniff and file
/// extension and dispatch to the matching importer; the single entry point for CLI and desktop.
/// Falls back to OpenAPI so its own error surfaces for unrecognised input.
///
/// `projectRoot` is the containment scope: `spec` must resolve to a file under it.
[[nodiscard]] std::expected<OpenApiImportOutcome, ReqloomError> importAny(
    const std::filesystem::path& spec, const std::filesystem::path& projectRoot);

}  // namespace reqloom::engine
