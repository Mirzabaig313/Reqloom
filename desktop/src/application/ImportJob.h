// ImportJob — the off-thread part of a desktop import: parse the file, check the target folder,
// write the project. Touches no shared state, so it can run on a worker.
#pragma once

#include <QtCore/QString>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {
class ProjectTrustStore;
class SecretStore;
}  // namespace reqloom::engine

namespace reqloom::desktop {

/// Result of an import job, handed back to the GUI thread.
struct ImportOutcome {
    enum class Status : std::uint8_t {
        Success,
        ImportFailed,    ///< the file could not be read or converted
        WriteFailed,     ///< the project could not be written
        NeedsOverwrite,  ///< a project exists there; ask, then run again with `overwrite`
        Refused,         ///< the target folder must not be written now; nothing was written
    };
    Status status{Status::ImportFailed};
    QString errorDetail;
    int resources{0};
    int operations{0};
    int movedCredentials{0};
    int keptCredentials{0};
    QString notes;
    QString loadDir;
    /// Echoed back so a NeedsOverwrite outcome can re-invoke the import with overwrite=true.
    QString specPath;
    QString baseDir;
};

/// An open project folder the import must not write into, and why (e.g. "has unsaved edits").
struct BlockedFolder {
    std::filesystem::path root;
    QString reason;
};

struct ImportRequest {
    std::filesystem::path spec;
    /// Folder that receives the project's own sub-folder; empty means the spec's folder.
    std::filesystem::path baseDir;
    bool overwrite{false};
    std::vector<BlockedFolder> blocked;
};

/// Import `request.spec` into `<baseDir>/<slug of the project name>`, moving credentials into
/// `store`. Never throws: an exception becomes a failed outcome carrying its message.
[[nodiscard]] ImportOutcome runImportJob(const ImportRequest& request,
                                         engine::SecretStore* store,
                                         engine::ProjectTrustStore* trust) noexcept;

/// `runImportJob` with the user's keychain and project trust store.
[[nodiscard]] ImportOutcome runImportJobWithUserStores(const ImportRequest& request) noexcept;

/// Filesystem-safe folder name: lowercase, non-alphanumeric runs become '-', trimmed;
/// "imported-project" if empty.
[[nodiscard]] std::string projectFolderSlug(std::string_view name);

}  // namespace reqloom::desktop
