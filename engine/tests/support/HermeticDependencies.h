// Engine dependencies for tests: the default HTTP, schema, history and hook adapters, with an
// in-memory secret store and trust store so no test reads the OS keychain or the user's
// trust file.
#pragma once

#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::tests {

class MemorySecretStore final : public engine::SecretStore {
public:
    explicit MemorySecretStore(std::map<std::string, std::string> values)
        : values_(std::move(values)) {}

    std::expected<std::optional<std::string>, engine::ReqloomError> read(
        const std::string& name) override {
        if (const auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, engine::ReqloomError> write(const std::string& name,
                                                    const std::string& value) override {
        values_[name] = value;
        return {};
    }
    std::expected<void, engine::ReqloomError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

private:
    std::map<std::string, std::string> values_;
};

class MemoryTrustStore final : public engine::ProjectTrustStore {
public:
    std::expected<std::optional<std::filesystem::path>, engine::ReqloomError> folderFor(
        const std::string& projectId) override {
        if (const auto it = folders_.find(projectId); it != folders_.end()) {
            return std::optional<std::filesystem::path>{it->second};
        }
        return std::optional<std::filesystem::path>{};
    }
    std::expected<std::filesystem::path, engine::ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) override {
        return folders_.try_emplace(projectId, folder).first->second;
    }
    std::expected<void, engine::ReqloomError> rebind(const std::string& projectId,
                                                     const std::filesystem::path& folder) override {
        folders_[projectId] = folder;
        return {};
    }

private:
    std::map<std::string, std::filesystem::path> folders_;
};

/// Host approvals kept in memory. `approveEveryHost` stands for a person who has approved every
/// host the tests send to (the mock server on 127.0.0.1); tests of host approval itself start
/// with nothing approved. Test-only: production wiring uses the file-backed store.
class MemoryHostApprovals final : public engine::HostApprovalStore {
public:
    explicit MemoryHostApprovals(bool approveEveryHost = false)
        : approveEveryHost_(approveEveryHost) {}

    std::expected<bool, engine::ReqloomError> isApproved(const engine::Project& project,
                                                         const std::string& hostKey) override {
        return approveEveryHost_ || hosts_.contains(project.id + " " + hostKey);
    }
    std::expected<void, engine::ReqloomError> approve(const engine::Project& project,
                                                      const std::string& hostKey) override {
        hosts_.insert(project.id + " " + hostKey);
        return {};
    }
    std::expected<void, engine::ReqloomError> revoke(const engine::Project& project,
                                                     const std::string& hostKey) override {
        hosts_.erase(project.id + " " + hostKey);
        return {};
    }
    std::expected<std::vector<std::string>, engine::ReqloomError> approvedHosts(
        const engine::Project& project) override {
        std::vector<std::string> out;
        for (const auto& entry : hosts_) {
            if (entry.starts_with(project.id + " ")) {
                out.push_back(entry.substr(project.id.size() + 1));
            }
        }
        return out;
    }
    std::expected<bool, engine::ReqloomError> cliApprovalAllowed() override { return cli_; }
    std::expected<void, engine::ReqloomError> setCliApprovalAllowed(bool allowed) override {
        cli_ = allowed;
        return {};
    }

private:
    bool approveEveryHost_;
    bool cli_{true};
    std::set<std::string> hosts_;
};

/// `deps` with every host approved, for tests that are not about host approval.
[[nodiscard]] inline engine::ExecutionEngine::Dependencies withApprovedHosts(
    engine::ExecutionEngine::Dependencies deps) {
    deps.hostApprovals = std::make_unique<MemoryHostApprovals>(/*approveEveryHost=*/true);
    return deps;
}

/// `makeDefaultDependencies()` with `secrets` (keyed `<project id>/<name>`) in memory.
[[nodiscard]] inline engine::ExecutionEngine::Dependencies makeHermeticDependencies(
    std::map<std::string, std::string> secrets = {}) {
    auto deps = engine::makeDefaultDependencies();
    deps.secrets = std::make_unique<MemorySecretStore>(std::move(secrets));
    deps.trust = std::make_unique<MemoryTrustStore>();
    deps.hostApprovals = std::make_unique<MemoryHostApprovals>(/*approveEveryHost=*/true);
    return deps;
}

}  // namespace reqloom::tests
