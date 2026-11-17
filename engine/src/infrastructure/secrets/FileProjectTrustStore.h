// FileProjectTrustStore — the per-user JSON file that records which folder
// each project id belongs to, and the hosts approved for it (reqloom/engine/ProjectTrustStore.h,
// reqloom/engine/HostApproval.h).
// Format: UTF-8 JSON `{ "version": 1, "settings": { "cli_host_approval": false },
// "projects": { "<id>": { "folder": "/abs/path", "approved_hosts": ["api.example.com:443"] } } }`;
// unknown keys are preserved on write. Reads treat a bad file as empty; writes lock, replace
// atomically and are user-only.
#pragma once

#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/ProjectTrustStore.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::engine {

class FileProjectTrustStore final : public ProjectTrustStore, public HostApprovalStore {
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

    std::expected<bool, ReqloomError> isApproved(const Project& project,
                                                 const std::string& hostKey) override;
    std::expected<void, ReqloomError> approve(const Project& project,
                                              const std::string& hostKey) override;
    std::expected<void, ReqloomError> revoke(const Project& project,
                                             const std::string& hostKey) override;
    std::expected<std::vector<std::string>, ReqloomError> approvedHosts(
        const Project& project) override;
    std::expected<bool, ReqloomError> cliApprovalAllowed() override;
    std::expected<void, ReqloomError> setCliApprovalAllowed(bool allowed) override;

    /// The per-platform default location (see `defaultProjectTrustStorePath`).
    [[nodiscard]] static std::expected<std::filesystem::path, ReqloomError> defaultPath();

private:
    std::filesystem::path file_;
    std::optional<ReqloomError> pathError_;
};

}  // namespace reqloom::engine
