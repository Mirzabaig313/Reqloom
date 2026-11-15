// Public interface for the per-user record of which folder each project id
// belongs to on this machine. Secrets are keyed by the project id, and the id
// comes from the project file, so a second project could copy another's id to
// read its secrets. The engine therefore binds each id to the folder that
// first used it (trust on first use) and refuses secrets to any other folder
// claiming that id. The record lives outside every project folder, in the
// user's application config directory, so a project cannot edit it.
//
// Embedders construct the file-backed store with `makeFileProjectTrustStore()`
// (Factories.h); tests inject an in-memory fake.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace reqloom::engine {

class ProjectTrustStore {
public:
    ProjectTrustStore() = default;
    ProjectTrustStore(const ProjectTrustStore&) = delete;
    ProjectTrustStore& operator=(const ProjectTrustStore&) = delete;
    ProjectTrustStore(ProjectTrustStore&&) = delete;
    ProjectTrustStore& operator=(ProjectTrustStore&&) = delete;
    virtual ~ProjectTrustStore() = default;

    /// The folder recorded for `projectId`, or an empty optional when the id
    /// has no record. `ReqloomError` (`SecretAccessFailed`) on I/O failure.
    [[nodiscard]] virtual std::expected<std::optional<std::filesystem::path>, ReqloomError>
    folderFor(const std::string& projectId) = 0;

    /// Record `folder` for `projectId` unless the id already has a record, in
    /// one atomic step (two processes racing cannot both win). Returns the
    /// folder on record afterwards: `folder` when it was recorded now, or the
    /// earlier record otherwise.
    [[nodiscard]] virtual std::expected<std::filesystem::path, ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) = 0;

    /// Replace the record for `projectId` with `folder`. Only for an explicit
    /// user action ("I moved this project"), never automatically.
    [[nodiscard]] virtual std::expected<void, ReqloomError> rebind(
        const std::string& projectId, const std::filesystem::path& folder) = 0;
};

}  // namespace reqloom::engine
