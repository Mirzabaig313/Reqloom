// Public interface for the per-user record binding each project id to the folder that first used it
// (trust on first use), so a project copying another's id is refused its secrets. The record lives
// in the user's application config directory, outside every project folder, so a project cannot
// edit it.
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

    /// Record `folder` for `projectId` unless it already has a record, in one atomic step; returns
    /// the folder on record afterwards.
    [[nodiscard]] virtual std::expected<std::filesystem::path, ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) = 0;

    /// Replace the record for `projectId` with `folder`. Only for an explicit
    /// user action ("I moved this project"), never automatically.
    [[nodiscard]] virtual std::expected<void, ReqloomError> rebind(
        const std::string& projectId, const std::filesystem::path& folder) = 0;
};

}  // namespace reqloom::engine
