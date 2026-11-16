// YamlSchemaWriter when other writers share the folder: unique staging files, pruning limited
// to the files a model loaded, collisions and on-disk changes refused.
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class ScratchDir {
public:
    ScratchDir() {
        path_ = reqloom::tests::uniqueTempPath("reqloom-writer-safety");
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

    fs::path write(const std::string& relPath, const std::string& body) const {
        const auto full = path_ / relPath;
        fs::create_directories(full.parent_path());
        std::ofstream out{full};
        out << body;
        return full;
    }

    [[nodiscard]] std::string read(const std::string& relPath) const {
        std::ifstream in{path_ / relPath};
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    fs::path path_;
};

ce::Project loadSeeded(const ScratchDir& scratch) {
    scratch.write("reqloom.yaml",
                  "version: 1\nname: Safety\nid: safety-test\ndefault_environment: local\n"
                  "imports: [actors/*.yaml, resources/*.yaml, environments/*.yaml]\n");
    scratch.write("resources/users.yaml",
                  "name: users\noperations:\n  list: { method: GET, path: /users }\n");
    auto project = ce::parseProject(scratch.path() / "reqloom.yaml");
    EXPECT_TRUE(project.has_value()) << project.error().detail;
    return project ? *project : ce::Project{};
}

void addResource(ce::Project& project, const std::string& id) {
    ce::Resource res;
    res.id = ce::ResourceId{id};
    ce::Operation op;
    op.id = ce::OperationId{id + ".list"};
    op.resource = res.id;
    op.pathTemplate = "/" + id;
    res.operations["list"] = op;
    project.resources[res.id] = res;
}

}  // namespace

TEST(SchemaWriterSafety, another_writers_staging_file_is_left_untouched) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    addResource(project, "orders");
    scratch.write("resources/orders.yaml.tmp", "someone else's staged bytes");

    ASSERT_TRUE(ce::writeProject(scratch.path(), project, /*overwrite=*/true).has_value());
    EXPECT_EQ(scratch.read("resources/orders.yaml.tmp"), "someone else's staged bytes");
    EXPECT_TRUE(fs::exists(scratch.path() / "resources" / "orders.yaml"));
}

TEST(SchemaWriterSafety, no_staging_file_is_left_after_a_failed_commit) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    addResource(project, "orders");
    // A directory where the target file should go makes the commit rename fail.
    fs::create_directories(scratch.path() / "resources" / "orders.yaml" / "blocker");

    ASSERT_FALSE(ce::writeProject(scratch.path(), project, /*overwrite=*/true).has_value());
    for (const auto& entry : fs::recursive_directory_iterator(scratch.path())) {
        EXPECT_NE(entry.path().extension(), ".tmp") << entry.path();
    }
}

TEST(SchemaWriterSafety, file_added_by_someone_else_is_not_pruned) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    scratch.write("resources/extra.yaml",
                  "name: extra\noperations:\n  list: { method: GET, path: /extra }\n");

    ASSERT_TRUE(ce::writeProject(scratch.path(),
                                 project,
                                 /*overwrite=*/true,
                                 /*pruneStale=*/true,
                                 &project.loadedFiles)
                    .has_value());
    EXPECT_TRUE(fs::exists(scratch.path() / "resources" / "extra.yaml"));
}

TEST(SchemaWriterSafety, loaded_file_dropped_from_the_model_is_pruned) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    project.resources.erase(ce::ResourceId{"users"});

    ASSERT_TRUE(ce::writeProject(scratch.path(),
                                 project,
                                 /*overwrite=*/true,
                                 /*pruneStale=*/true,
                                 &project.loadedFiles)
                    .has_value());
    EXPECT_FALSE(fs::exists(scratch.path() / "resources" / "users.yaml"));
}

TEST(SchemaWriterSafety, creating_a_file_someone_else_added_is_an_error) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    scratch.write("resources/orders.yaml", "name: orders\n# written by an agent\n");
    addResource(project, "orders");

    auto written = ce::writeProject(
        scratch.path(), project, /*overwrite=*/true, /*pruneStale=*/true, &project.loadedFiles);
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().detail.find("orders.yaml"), std::string::npos)
        << written.error().detail;
    EXPECT_EQ(scratch.read("resources/orders.yaml"), "name: orders\n# written by an agent\n");
}

TEST(SchemaWriterSafety, file_changed_on_disk_since_load_is_an_error) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    const std::string edited =
        "name: users\noperations:\n  list: { method: GET, path: /users/v2 }\n";
    scratch.write("resources/users.yaml", edited);
    project.resources.at(ce::ResourceId{"users"}).operations.at("list").pathTemplate = "/mine";

    auto written = ce::writeProject(
        scratch.path(), project, /*overwrite=*/true, /*pruneStale=*/true, &project.loadedFiles);
    ASSERT_FALSE(written.has_value());
    EXPECT_NE(written.error().detail.find("changed on disk"), std::string::npos)
        << written.error().detail;
    EXPECT_EQ(scratch.read("resources/users.yaml"), edited);
}

TEST(SchemaWriterSafety, loaded_set_is_updated_so_the_next_save_works) {
    ScratchDir scratch;
    auto project = loadSeeded(scratch);
    addResource(project, "orders");
    ASSERT_TRUE(ce::writeProject(scratch.path(),
                                 project,
                                 /*overwrite=*/true,
                                 /*pruneStale=*/true,
                                 &project.loadedFiles)
                    .has_value());

    // Rename the new resource: the second save must replace orders.yaml, not refuse it.
    auto orders = project.resources.at(ce::ResourceId{"orders"});
    project.resources.erase(ce::ResourceId{"orders"});
    orders.id = ce::ResourceId{"purchases"};
    project.resources[orders.id] = orders;
    auto second = ce::writeProject(
        scratch.path(), project, /*overwrite=*/true, /*pruneStale=*/true, &project.loadedFiles);
    ASSERT_TRUE(second.has_value()) << second.error().detail;
    EXPECT_FALSE(fs::exists(scratch.path() / "resources" / "orders.yaml"));
    EXPECT_TRUE(fs::exists(scratch.path() / "resources" / "purchases.yaml"));
}
