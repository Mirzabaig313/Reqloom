// ProjectTrustStoreTest — the per-user record binding each project id to the
// folder that first used it, and the rule `checkProjectFolder` applies before
// any secret is read or written. Uses the real file-backed store pointed at a
// temp file, so the on-disk format and its failure handling are covered.
#include "support/TempPath.h"

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectIdentity.h>
#include <reqloom/engine/ProjectTrustStore.h>

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

/// Sets (or, with nullopt, unsets) REQLOOM_CONFIG_DIR for one test and
/// restores the previous value at scope exit.
class ScopedConfigDirOverride {
public:
    explicit ScopedConfigDirOverride(const std::optional<std::string>& value) {
        if (const char* old = std::getenv(kVar); old != nullptr) {
            previous_ = old;
        }
        apply(value);
    }
    ~ScopedConfigDirOverride() { apply(previous_); }
    ScopedConfigDirOverride(const ScopedConfigDirOverride&) = delete;
    ScopedConfigDirOverride& operator=(const ScopedConfigDirOverride&) = delete;
    ScopedConfigDirOverride(ScopedConfigDirOverride&&) = delete;
    ScopedConfigDirOverride& operator=(ScopedConfigDirOverride&&) = delete;

private:
    static constexpr const char* kVar = "REQLOOM_CONFIG_DIR";

    static void apply(const std::optional<std::string>& value) {
#if defined(_WIN32)
        _putenv_s(kVar, value ? value->c_str() : "");
#else
        if (value) {
            setenv(kVar, value->c_str(), 1);
        } else {
            unsetenv(kVar);
        }
#endif
    }

    std::optional<std::string> previous_;
};

/// A scratch directory removed at scope exit, holding the trust file and
/// any number of project folders.
class Scratch {
public:
    Scratch() : root_(reqloom::tests::uniqueTempPath("reqloom-trust")) {
        fs::create_directories(root_);
        root_ = fs::canonical(root_);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    [[nodiscard]] fs::path trustFile() const { return root_ / "config" / "projects.json"; }

    [[nodiscard]] fs::path folder(const std::string& name) const {
        auto dir = root_ / name;
        fs::create_directories(dir);
        return dir;
    }

private:
    fs::path root_;
};

[[nodiscard]] ce::Project projectAt(const fs::path& folder, const std::string& id = "proj-a") {
    ce::Project project;
    project.name = "Trust";
    project.id = id;
    project.rootDir = folder;
    return project;
}

[[nodiscard]] std::string slurp(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST(ProjectTrustStore, first_use_records_the_folder) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    const auto folder = scratch.folder("a");

    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folder)).has_value());

    auto recorded = store->folderFor("proj-a");
    ASSERT_TRUE(recorded.has_value()) << recorded.error().detail;
    ASSERT_TRUE(recorded->has_value());
    EXPECT_EQ(**recorded, fs::canonical(folder));

    // A fresh store over the same file sees the record (it was persisted).
    auto reopened = ce::makeFileProjectTrustStore(scratch.trustFile());
    auto again = reopened->folderFor("proj-a");
    ASSERT_TRUE(again.has_value() && again->has_value());
    EXPECT_EQ(**again, fs::canonical(folder));
}

TEST(ProjectTrustStore, same_folder_passes_on_later_checks) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    const auto folder = scratch.folder("a");

    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folder)).has_value());
    auto second = ce::checkProjectFolder(store.get(), projectAt(folder));
    EXPECT_TRUE(second.has_value()) << second.error().detail;
}

TEST(ProjectTrustStore, different_folder_is_refused_naming_both_paths) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    const auto folderA = scratch.folder("a");
    const auto folderB = scratch.folder("b");

    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folderA)).has_value());
    auto refused = ce::checkProjectFolder(store.get(), projectAt(folderB));

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ce::ErrorCode::SecretAccessFailed);
    const auto& detail = refused.error().detail;
    EXPECT_NE(detail.find(fs::canonical(folderA).string()), std::string::npos) << detail;
    EXPECT_NE(detail.find(fs::canonical(folderB).string()), std::string::npos) << detail;
    EXPECT_NE(detail.find("desktop app"), std::string::npos) << detail;
    EXPECT_NE(detail.find("id:"), std::string::npos) << detail;

    // The refusal does not move the record.
    auto recorded = store->folderFor("proj-a");
    ASSERT_TRUE(recorded.has_value() && recorded->has_value());
    EXPECT_EQ(**recorded, fs::canonical(folderA));

    const auto conflict = ce::projectFolderConflict(*store, projectAt(folderB));
    ASSERT_TRUE(conflict.has_value());
    EXPECT_EQ(*conflict, fs::canonical(folderA));
    EXPECT_FALSE(ce::projectFolderConflict(*store, projectAt(folderA)).has_value());
}

TEST(ProjectTrustStore, rebind_moves_the_record_to_the_new_folder) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    const auto folderA = scratch.folder("a");
    const auto folderB = scratch.folder("b");

    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folderA)).has_value());
    ASSERT_TRUE(ce::rebindProjectFolder(*store, projectAt(folderB)).has_value());

    EXPECT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folderB)).has_value());
    EXPECT_FALSE(ce::checkProjectFolder(store.get(), projectAt(folderA)).has_value());
}

TEST(ProjectTrustStore, corrupt_file_is_treated_as_empty) {
    Scratch scratch;
    fs::create_directories(scratch.trustFile().parent_path());
    {
        std::ofstream out{scratch.trustFile(), std::ios::binary};
        out << "{ this is not json";
    }
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());

    auto none = store->folderFor("proj-a");
    ASSERT_TRUE(none.has_value()) << none.error().detail;
    EXPECT_FALSE(none->has_value());

    const auto folder = scratch.folder("a");
    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(folder)).has_value());
    const auto doc = nlohmann::json::parse(slurp(scratch.trustFile()));
    EXPECT_EQ(doc.at("version"), 1);
    EXPECT_EQ(doc.at("projects").at("proj-a").at("folder").get<std::string>(),
              fs::canonical(folder).string());
}

TEST(ProjectTrustStore, wrong_shape_json_is_treated_as_empty) {
    Scratch scratch;
    fs::create_directories(scratch.trustFile().parent_path());
    {
        std::ofstream out{scratch.trustFile(), std::ios::binary};
        out << R"({"version":1,"projects":{"proj-a":{"folder":42}, "proj-b": "x"}})";
    }
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    auto a = store->folderFor("proj-a");
    ASSERT_TRUE(a.has_value());
    EXPECT_FALSE(a->has_value());
    auto b = store->folderFor("proj-b");
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(b->has_value());
}

TEST(ProjectTrustStore, unknown_sections_survive_a_write) {
    // Later features add sections (per-project approved hosts, a top-level
    // block); an older build writing a record must keep them.
    Scratch scratch;
    fs::create_directories(scratch.trustFile().parent_path());
    {
        std::ofstream out{scratch.trustFile(), std::ios::binary};
        out << R"({"version":1,"future":{"k":true},)"
               R"("projects":{"proj-x":{"folder":"/somewhere","approved_hosts":["a.test"]}}})";
    }
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(scratch.folder("a"))).has_value());

    const auto doc = nlohmann::json::parse(slurp(scratch.trustFile()));
    EXPECT_EQ(doc.at("future").at("k"), true);
    EXPECT_EQ(doc.at("projects").at("proj-x").at("approved_hosts").at(0), "a.test");
    EXPECT_EQ(doc.at("projects").at("proj-x").at("folder"), "/somewhere");
    EXPECT_TRUE(doc.at("projects").contains("proj-a"));
}

TEST(ProjectTrustStore, symlinked_path_equals_its_real_path) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    const auto real = scratch.folder("real");
    const auto link = real.parent_path() / "link";
    std::error_code ec;
    fs::create_directory_symlink(real, link, ec);
    if (ec) {
        GTEST_SKIP() << "symlinks unavailable here: " << ec.message();
    }

    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(link)).has_value());
    auto viaReal = ce::checkProjectFolder(store.get(), projectAt(real));
    EXPECT_TRUE(viaReal.has_value()) << viaReal.error().detail;
    auto recorded = store->folderFor("proj-a");
    ASSERT_TRUE(recorded.has_value() && recorded->has_value());
    EXPECT_EQ(**recorded, fs::canonical(real));
}

TEST(ProjectTrustStore, project_without_a_folder_is_refused) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    auto refused = ce::checkProjectFolder(store.get(), projectAt(fs::path{}));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ce::ErrorCode::SecretAccessFailed);
    EXPECT_FALSE(fs::exists(scratch.trustFile())) << "nothing to record without a folder";
}

TEST(ProjectTrustStore, missing_store_is_refused_not_skipped) {
    Scratch scratch;
    auto refused = ce::checkProjectFolder(nullptr, projectAt(scratch.folder("a")));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ce::ErrorCode::SecretAccessFailed);
}

TEST(ProjectTrustStore, project_without_id_reports_the_id_problem_first) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    auto refused = ce::checkProjectFolder(store.get(), projectAt(scratch.folder("a"), ""));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_NE(refused.error().detail.find("reqloom.yaml"), std::string::npos);
}

TEST(ProjectTrustStore, concurrent_writers_keep_every_record) {
    // Each writer is its own store instance (as two processes would be);
    // the lock + atomic replace must not lose or corrupt records.
    Scratch scratch;
    constexpr int kWriters = 8;
    std::vector<fs::path> folders;
    for (int i = 0; i < kWriters; ++i) {
        folders.push_back(scratch.folder("p" + std::to_string(i)));
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < kWriters; ++i) {
        threads.emplace_back([&, i] {
            auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
            auto project =
                projectAt(folders[static_cast<std::size_t>(i)], "id-" + std::to_string(i));
            (void)ce::checkProjectFolder(store.get(), project);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    for (int i = 0; i < kWriters; ++i) {
        auto recorded = store->folderFor("id-" + std::to_string(i));
        ASSERT_TRUE(recorded.has_value() && recorded->has_value()) << i;
        EXPECT_EQ(**recorded, fs::canonical(folders[static_cast<std::size_t>(i)]));
    }
}

#ifndef _WIN32
TEST(ProjectTrustStore, file_is_readable_by_the_user_only) {
    Scratch scratch;
    auto store = ce::makeFileProjectTrustStore(scratch.trustFile());
    ASSERT_TRUE(ce::checkProjectFolder(store.get(), projectAt(scratch.folder("a"))).has_value());
    const auto perms = fs::status(scratch.trustFile()).permissions();
    EXPECT_EQ(perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
}
#endif

TEST(ProjectTrustStore, default_path_is_outside_any_project_and_platform_specific) {
    const ScopedConfigDirOverride unset{std::nullopt};
    auto path = ce::defaultProjectTrustStorePath();
    ASSERT_TRUE(path.has_value()) << path.error().detail;
    EXPECT_EQ(path->filename(), "projects.json");
#if defined(__APPLE__)
    EXPECT_NE(path->string().find("Library/Application Support/Reqloom"), std::string::npos);
#elif defined(_WIN32)
    EXPECT_EQ(path->parent_path().filename(), "Reqloom");
#else
    EXPECT_EQ(path->parent_path().filename(), "reqloom");
#endif
}

TEST(ProjectTrustStore, config_dir_override_replaces_the_platform_directory) {
    const fs::path dir = reqloom::tests::uniqueTempPath("reqloom-config-override");
    const ScopedConfigDirOverride override{dir.string()};

    auto path = ce::defaultProjectTrustStorePath();

    ASSERT_TRUE(path.has_value()) << path.error().detail;
    EXPECT_EQ(*path, dir / "projects.json");
}

TEST(ProjectTrustStore, empty_config_dir_override_is_ignored) {
    const ScopedConfigDirOverride empty{std::string{}};

    auto path = ce::defaultProjectTrustStorePath();

    ASSERT_TRUE(path.has_value()) << path.error().detail;
    EXPECT_TRUE(path->is_absolute()) << path->string();
}
