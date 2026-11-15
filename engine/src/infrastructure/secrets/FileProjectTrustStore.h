// FileProjectTrustStore — the per-user JSON file that records which folder
// each project id belongs to. See reqloom/engine/ProjectTrustStore.h.
//
// Format (UTF-8 JSON, unknown keys preserved on every write so later
// features can add sections without a format break):
//
//   {
//     "version": 1,
//     "projects": {
//       "<project id>": { "folder": "/absolute/canonical/path" }
//     }
//   }
//
// Reads tolerate a missing, unparseable or wrongly-shaped file (it is treated
// as empty). Writes take a lock file next to it (two processes cannot
// interleave a read-modify-write), replace the file atomically (temp file +
// rename) and create it readable by the user only.
#pragma once

#include <reqloom/engine/ProjectTrustStore.h>

#include <filesystem>
#include <optional>
#include <string>

namespace reqloom::engine {

class FileProjectTrustStore final : public ProjectTrustStore {
public:
    explicit FileProjectTrustStore(std::filesystem::path file);
    /// A store whose path could not be resolved: every call returns `error`.
    explicit FileProjectTrustStore(ReqloomError error);
    ~FileProjectTrustStore() override;

    FileProjectTrustStore(const FileProjectTrustStore&) = delete;
    FileProjectTrustStore& operator=(const FileProjectTrustStore&) = delete;
    FileProjectTrustStore(FileProjectTrustStore&&) = delete;
    FileProjectTrustStore& operator=(FileProjectTrustStore&&) = delete;

    std::expected<std::optional<std::filesystem::path>, ReqloomError> folderFor(
        const std::string& projectId) override;
    std::expected<std::filesystem::path, ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) override;
    std::expected<void, ReqloomError> rebind(const std::string& projectId,
                                             const std::filesystem::path& folder) override;

    /// The per-platform default location (see `defaultProjectTrustStorePath`).
    [[nodiscard]] static std::expected<std::filesystem::path, ReqloomError> defaultPath();

private:
    std::filesystem::path file_;
    std::optional<ReqloomError> pathError_;
};

}  // namespace reqloom::engine
