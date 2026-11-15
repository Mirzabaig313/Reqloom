// Tests the off-thread import job: it asks before replacing a project, refuses folders the app
// must not write into, and turns any exception into a failed outcome. In-memory secret and trust
// stores only; temp dirs only.
#include "application/ImportJob.h"

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>

#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

class MemoryStore final : public ce::SecretStore {
public:
    bool throwOnRead{false};
    std::map<std::string, std::string> values;

    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& key) override {
        if (throwOnRead) {
            throw std::runtime_error("keychain exploded");
        }
        if (const auto it = values.find(key); it != values.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, ce::ReqloomError> write(const std::string& key,
                                                const std::string& value) override {
        values[key] = value;
        return {};
    }
    std::expected<void, ce::ReqloomError> remove(const std::string& key) override {
        values.erase(key);
        return {};
    }
};

class MemoryTrust final : public ce::ProjectTrustStore {
public:
    std::map<std::string, fs::path> folders;

    std::expected<std::optional<fs::path>, ce::ReqloomError> folderFor(
        const std::string& id) override {
        if (const auto it = folders.find(id); it != folders.end()) {
            return std::optional<fs::path>{it->second};
        }
        return std::optional<fs::path>{};
    }
    std::expected<fs::path, ce::ReqloomError> recordIfAbsent(const std::string& id,
                                                             const fs::path& folder) override {
        return folders.try_emplace(id, folder).first->second;
    }
    std::expected<void, ce::ReqloomError> rebind(const std::string& id,
                                                 const fs::path& folder) override {
        folders[id] = folder;
        return {};
    }
};

// A Postman collection named "Shop" (project folder "shop") with a pasted bearer token.
constexpr auto kCollection = R"JSON(
{
  "info": { "name": "Shop", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    { "name": "One", "request": { "method": "GET", "url": "https://api.example.com/one",
      "header": [ { "key": "Authorization", "value": "Bearer abc123def456" } ] } }
  ]
}
)JSON";

[[nodiscard]] fs::path dirOf(const QTemporaryDir& dir) {
    return fs::path{dir.path().toStdString()};
}

[[nodiscard]] fs::path writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << text;
    return path;
}

[[nodiscard]] std::string readFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] ImportRequest requestFor(const QTemporaryDir& dir, bool overwrite) {
    ImportRequest request;
    request.spec = writeFile(dirOf(dir) / "shop.json", kCollection);
    request.overwrite = overwrite;
    return request;
}

}  // namespace

TEST(ImportJob, writes_a_new_project_and_reports_what_it_imported) {
    QTemporaryDir dir;
    MemoryStore store;
    MemoryTrust trust;
    const ImportOutcome out = runImportJob(requestFor(dir, false), &store, &trust);
    ASSERT_EQ(out.status, ImportOutcome::Status::Success) << out.errorDetail.toStdString();
    EXPECT_EQ(out.resources, 1);
    EXPECT_EQ(out.operations, 1);
    EXPECT_EQ(out.movedCredentials, 1);
    EXPECT_TRUE(fs::exists(dirOf(dir) / "shop" / "reqloom.yaml"));
}

TEST(ImportJob, an_existing_project_is_not_touched_without_confirmation) {
    QTemporaryDir dir;
    const auto yaml = writeFile(dirOf(dir) / "shop" / "reqloom.yaml", "version: 1\nname: Mine\n");
    MemoryStore store;
    MemoryTrust trust;
    const ImportOutcome out = runImportJob(requestFor(dir, false), &store, &trust);
    EXPECT_EQ(out.status, ImportOutcome::Status::NeedsOverwrite);
    EXPECT_EQ(readFile(yaml), "version: 1\nname: Mine\n");
    EXPECT_TRUE(store.values.empty());
}

TEST(ImportJob, a_blocked_open_project_is_refused_before_anything_is_written) {
    QTemporaryDir dir;
    const auto yaml = writeFile(dirOf(dir) / "shop" / "reqloom.yaml", "version: 1\nname: Mine\n");
    MemoryStore store;
    MemoryTrust trust;
    ImportRequest request = requestFor(dir, true);
    request.blocked.push_back({dirOf(dir) / "shop", QStringLiteral("has unsaved edits")});

    const ImportOutcome out = runImportJob(request, &store, &trust);
    EXPECT_EQ(out.status, ImportOutcome::Status::Refused);
    EXPECT_TRUE(out.errorDetail.contains(QStringLiteral("has unsaved edits")));
    EXPECT_EQ(readFile(yaml), "version: 1\nname: Mine\n");
    EXPECT_TRUE(store.values.empty());
}

TEST(ImportJob, a_folder_with_other_files_and_no_project_is_refused) {
    QTemporaryDir dir;
    writeFile(dirOf(dir) / "shop" / "notes.txt", "mine");
    MemoryStore store;
    MemoryTrust trust;
    const ImportOutcome out = runImportJob(requestFor(dir, false), &store, &trust);
    EXPECT_EQ(out.status, ImportOutcome::Status::Refused);
    EXPECT_FALSE(fs::exists(dirOf(dir) / "shop" / "reqloom.yaml"));
}

TEST(ImportJob, an_exception_inside_the_job_becomes_a_failed_outcome) {
    QTemporaryDir dir;
    MemoryStore store;
    store.throwOnRead = true;
    MemoryTrust trust;
    const ImportOutcome out = runImportJob(requestFor(dir, false), &store, &trust);
    EXPECT_EQ(out.status, ImportOutcome::Status::WriteFailed);
    EXPECT_TRUE(out.errorDetail.contains(QStringLiteral("keychain exploded")));
}

TEST(ImportJob, an_unreadable_spec_is_an_import_failure) {
    QTemporaryDir dir;
    ImportRequest request;
    request.spec = writeFile(dirOf(dir) / "broken.json", "{ not json");
    const ImportOutcome out = runImportJob(request, nullptr, nullptr);
    EXPECT_EQ(out.status, ImportOutcome::Status::ImportFailed);
    EXPECT_FALSE(out.errorDetail.isEmpty());
}

}  // namespace reqloom::desktop::tests
