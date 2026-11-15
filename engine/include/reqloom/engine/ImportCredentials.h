// Credentials found in an imported collection, and moving them into the project's secrets
// before the project is written, so an import puts nothing sensitive in the YAML.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace reqloom::engine {

class ProjectTrustStore;
class SecretStore;

/// An environment variable the source format itself marks as secret (Postman `type: secret`,
/// HTTPie `isSecret`). `variable` is the key as it appears in the imported environment.
struct SourceSecretVariable {
    std::string environment;
    std::string variable;
};

/// One place in an imported project that holds a credential.
struct CredentialSite {
    enum class Kind : std::uint8_t { Header, EnvironmentVariable };
    Kind kind{Kind::Header};
    std::string resource;   ///< Header: the operation's resource id.
    std::string operation;  ///< Header: the operation's key within the resource.
    std::string header;     ///< Header: the header name as spelled in the project.
    std::string prefix;     ///< Header: the text before the credential (`Bearer `).
    std::string environment;
    std::string variable;
};

/// A credential to move into secrets: one value, the secret name it would get, and every
/// place it appears. The project still holds the plain value at each site.
struct ImportedCredential {
    std::string name;
    std::string value;
    std::vector<CredentialSite> sites;
};

/// Find the credentials in an imported project. Exactly these count, and nothing else:
///  - the value of an environment variable listed in `flagged` (the source marked it secret);
///  - the credential part of a literal `Authorization` / `Proxy-Authorization` header (after
///    the scheme word, or the whole value when it has none) and a whole literal `X-API-Key`.
/// A value holding a `{{…}}` reference is not literal and is skipped; so is an empty one.
///
/// Names are upper snake case and unique: a flagged variable's own name (`<ENV>_<VAR>` when
/// its value differs between environments); a header value used by one operation
/// `<RESOURCE>_<OPERATION>_<HEADER>`, by several `<PROJECT NAME>_<HEADER>`.
[[nodiscard]] std::vector<ImportedCredential> detectImportedCredentials(
    const Project& project, const std::vector<SourceSecretVariable>& flagged);

/// A credential that stayed in the YAML in plain text, and why.
struct KeptCredential {
    std::string name;
    std::string where;   ///< e.g. "operation orders.create header `Authorization`"
    std::string reason;  ///< never contains the credential value
};

/// What happened to each detected credential.
struct CredentialMoveReport {
    std::vector<std::string> moved;  ///< secret names the project now references
    std::vector<KeptCredential> kept;
    std::vector<std::string> newlyStoredKeys;  ///< store keys this import wrote

    /// One warning line per kept credential, saying how to move it into secrets later.
    [[nodiscard]] std::vector<std::string> warnings() const;
};

/// Move `credentials` into `store` for the project that will be written to `targetDir`.
///
/// Gives `project` an id first when it has none (the one in `targetDir/reqloom.yaml`, else a
/// new one) and runs the folder check for `targetDir`. Each credential that is stored, or
/// already stored with the same value, is replaced in `project` by `{{secret.NAME}}`. One
/// that cannot be stored (no store, no trust store, a refused folder check, a store error, or
/// an existing secret of that name with another value, which is never overwritten) stays in
/// `project` exactly as it was and is listed in `kept`. Nothing is lost either way.
[[nodiscard]] CredentialMoveReport moveImportedCredentials(
    Project& project,
    const std::vector<ImportedCredential>& credentials,
    SecretStore* store,
    ProjectTrustStore* trust,
    const std::filesystem::path& targetDir);

/// Options for `writeImportedProject`.
struct ImportWriteOptions {
    bool overwrite{false};
    bool moveCredentials{true};  ///< false keeps every credential in the YAML, with a warning
};

/// Result of `writeImportedProject`.
struct ImportWriteResult {
    std::filesystem::path reqloomYaml;
    CredentialMoveReport credentials;
};

/// Write an imported project into `targetDir` (never pruning other files), first moving its
/// credentials into secrets as `moveImportedCredentials` does. Refuses an existing
/// reqloom.yaml without `overwrite` before storing anything. If the write fails, the secrets
/// this import stored are removed again and the error says whether that succeeded.
[[nodiscard]] std::expected<ImportWriteResult, ReqloomError> writeImportedProject(
    const std::filesystem::path& targetDir,
    Project project,
    const std::vector<ImportedCredential>& credentials,
    SecretStore* store,
    ProjectTrustStore* trust,
    const ImportWriteOptions& options);

}  // namespace reqloom::engine
