// Tests SecretManager against an in-memory SecretStore (no real keychain).
// Confirms it reports per-secret presence for a project's {{secret.NAME}}
// references, and that store/clear round-trip through the injected store.
#include "application/ProjectModel.h"
#include "application/SecretManager.h"
#include "application/SecretReveal.h"

#include <reqloom/engine/Factories.h>

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;

/// In-memory SecretStore standing in for the OS keychain.
class FakeSecretStore final : public ce::SecretStore {
public:
    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        ++reads;
        if (failReads) {
            return std::unexpected(
                ce::ReqloomError{ce::ErrorCode::SecretAccessFailed, ce::ErrorClass::Run, "locked"});
        }
        if (auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }

    std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                const std::string& value) override {
        ++writes;
        values_[name] = value;
        return {};
    }

    std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

    [[nodiscard]] bool has(const std::string& name) const { return values_.contains(name); }

    bool failReads{false};
    int reads{0};
    int writes{0};

private:
    std::map<std::string, std::string> values_;
};

/// In-memory ProjectTrustStore standing in for the per-user trust file.
class FakeTrustStore final : public ce::ProjectTrustStore {
public:
    std::expected<std::optional<std::filesystem::path>, ce::ReqloomError> folderFor(
        const std::string& projectId) override {
        if (auto it = folders_.find(projectId); it != folders_.end()) {
            return std::optional<std::filesystem::path>{it->second};
        }
        return std::optional<std::filesystem::path>{};
    }

    std::expected<std::filesystem::path, ce::ReqloomError> recordIfAbsent(
        const std::string& projectId, const std::filesystem::path& folder) override {
        return folders_.try_emplace(projectId, folder).first->second;
    }

    std::expected<void, ce::ReqloomError> rebind(const std::string& projectId,
                                                 const std::filesystem::path& folder) override {
        folders_[projectId] = folder;
        return {};
    }

private:
    std::map<std::string, std::filesystem::path> folders_;
};

/// Build a SecretManager wrapping a FakeSecretStore we retain a handle to.
struct ManagerWithFake {
    FakeSecretStore* fake{nullptr};
    std::unique_ptr<SecretManager> manager;
};

[[nodiscard]] ManagerWithFake makeManager() {
    auto store = std::make_unique<FakeSecretStore>();
    auto* raw = store.get();
    return ManagerWithFake{raw,
                           std::make_unique<SecretManager>(
                               std::move(store), std::make_unique<FakeTrustStore>(), nullptr)};
}

// A minimal valid project whose operation references two secrets via
// {{secret.X}} — the form collectSecretReferences enumerates.
constexpr const char* kProjectYaml = R"(version: 1
name: Secret Refs
default_environment: local

environments:
  local:
    baseUrl: http://placeholder.invalid

actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /login
      body:
        email: "u@x.test"
        password: "p"
      expect_status: 200
      extract:
        token: $.accessToken
    inject:
      headers:
        Authorization: "Bearer {{user.token}}"

resources:
  thing:
    operations:
      get:
        method: GET
        path: /api/thing
        actor: user
        headers:
          X-Api-Key: "{{secret.API_KEY}}"
          X-Signature: "{{secret.SIGNING_KEY}}"
)";

/// Write kProjectYaml into a temp dir and load it through ProjectModel.
/// The QTemporaryDir is returned so it outlives the loaded project.
void writeProject(const QTemporaryDir& dir, const char* extraRootKeys = "") {
    const QString path = QDir(dir.path()).filePath(QStringLiteral("reqloom.yaml"));
    std::ofstream out(path.toStdString());
    out << extraRootKeys << kProjectYaml;
}

}  // namespace

TEST(SecretManager, store_then_clear_round_trips_through_backend) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-a\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    ASSERT_TRUE(project.hasProject());

    auto [fake, manager] = makeManager();

    QString error;
    ASSERT_TRUE(manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("s3cr3t"), error))
        << error.toStdString();
    EXPECT_TRUE(fake->has("proj-a/API_KEY"));
    EXPECT_FALSE(fake->has("API_KEY"));

    ASSERT_TRUE(manager->clear(project, QStringLiteral("API_KEY"), error)) << error.toStdString();
    EXPECT_FALSE(fake->has("proj-a/API_KEY"));
}

TEST(SecretManager, store_refuses_an_empty_value_and_writes_nothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-a\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    ASSERT_TRUE(project.hasProject());
    auto [fake, manager] = makeManager();

    QString error;
    const bool stored = manager->store(project, QStringLiteral("API_KEY"), QString{}, error);

    EXPECT_FALSE(stored);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(fake->has("proj-a/API_KEY"));
}

TEST(SecretManager, confirm_move_refuses_when_the_recorded_folder_is_not_the_one_shown) {
    QTemporaryDir original;
    QTemporaryDir moved;
    ASSERT_TRUE(original.isValid());
    ASSERT_TRUE(moved.isValid());
    writeProject(original, "id: shared\n");
    writeProject(moved, "id: shared\n");
    ProjectModel atOriginal;
    ProjectModel atMoved;
    atOriginal.loadFromDirectory(original.path());
    atMoved.loadFromDirectory(moved.path());
    ASSERT_TRUE(atOriginal.hasProject());
    ASSERT_TRUE(atMoved.hasProject());
    auto [fake, manager] = makeManager();
    ASSERT_EQ(manager->referencedSecrets(atOriginal).front().state, SecretState::NotSet);
    const QString recorded = manager->folderConflict(atMoved);
    ASSERT_FALSE(recorded.isEmpty());

    QString error;
    const bool movedToStale =
        manager->confirmMove(atMoved, QDir(moved.path()).filePath(QStringLiteral("x")), error);

    EXPECT_FALSE(movedToStale);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(manager->folderConflict(atMoved), recorded);
    ASSERT_TRUE(manager->confirmMove(atMoved, recorded, error)) << error.toStdString();
    EXPECT_TRUE(manager->folderConflict(atMoved).isEmpty());
}

TEST(SecretManager, projects_with_different_ids_see_only_their_own_secrets) {
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    ASSERT_TRUE(dirA.isValid());
    ASSERT_TRUE(dirB.isValid());
    writeProject(dirA, "id: proj-a\n");
    writeProject(dirB, "id: proj-b\n");
    ProjectModel projectA;
    ProjectModel projectB;
    projectA.loadFromDirectory(dirA.path());
    projectB.loadFromDirectory(dirB.path());
    ASSERT_TRUE(projectA.hasProject());
    ASSERT_TRUE(projectB.hasProject());

    auto [fake, manager] = makeManager();
    QString error;
    ASSERT_TRUE(manager->store(projectA, QStringLiteral("API_KEY"), QStringLiteral("a"), error))
        << error.toStdString();

    const auto seenByA = manager->referencedSecrets(projectA);
    const auto seenByB = manager->referencedSecrets(projectB);
    ASSERT_EQ(seenByA.size(), 2);
    ASSERT_EQ(seenByB.size(), 2);
    EXPECT_EQ(seenByA[0].state, SecretState::Set);
    EXPECT_EQ(seenByB[0].state, SecretState::NotSet);

    ASSERT_TRUE(manager->clear(projectB, QStringLiteral("API_KEY"), error)) << error.toStdString();
    EXPECT_TRUE(fake->has("proj-a/API_KEY"));
}

TEST(SecretManager, opening_a_project_without_id_assigns_and_persists_one) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir);

    ProjectModel project;
    project.loadFromDirectory(dir.path());
    ASSERT_TRUE(project.hasProject());
    const auto assigned = project.project().id;
    EXPECT_FALSE(assigned.empty());

    ProjectModel reopened;
    reopened.loadFromDirectory(dir.path());
    ASSERT_TRUE(reopened.hasProject());
    EXPECT_EQ(reopened.project().id, assigned);
}

TEST(SecretManager, referenced_secrets_reports_set_and_unset_against_project) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir);

    ProjectModel project;
    project.loadFromDirectory(dir.path());
    ASSERT_TRUE(project.hasProject());

    auto [fake, manager] = makeManager();

    // Both referenced secrets start unset, and are reported sorted.
    auto before = manager->referencedSecrets(project);
    ASSERT_EQ(before.size(), 2);
    EXPECT_EQ(before[0].name, QStringLiteral("API_KEY"));
    EXPECT_EQ(before[1].name, QStringLiteral("SIGNING_KEY"));
    EXPECT_EQ(before[0].state, SecretState::NotSet);
    EXPECT_EQ(before[1].state, SecretState::NotSet);

    // Set one; it flips to Set, the other stays NotSet.
    QString error;
    ASSERT_TRUE(manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("v"), error))
        << error.toStdString();

    auto after = manager->referencedSecrets(project);
    ASSERT_EQ(after.size(), 2);
    EXPECT_EQ(after[0].name, QStringLiteral("API_KEY"));
    EXPECT_EQ(after[0].state, SecretState::Set);
    EXPECT_EQ(after[1].name, QStringLiteral("SIGNING_KEY"));
    EXPECT_EQ(after[1].state, SecretState::NotSet);
}

// ─── Reveal (desktop only): the stored value on demand, never kept once hidden ───────────────

namespace {

/// Every file under `dir` with its size, to show a reveal stored nothing.
[[nodiscard]] std::map<std::string, std::uintmax_t> filesUnder(const std::filesystem::path& dir) {
    std::map<std::string, std::uintmax_t> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            out[entry.path().string()] = entry.file_size(ec);
        }
    }
    return out;
}

}  // namespace

TEST(SecretReveal, nothing_is_read_until_asked_and_each_show_reads_the_keychain_again) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-r\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    ASSERT_TRUE(project.hasProject());
    auto [fake, manager] = makeManager();
    QString error;
    ASSERT_TRUE(
        manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("first-1"), error));
    const int readsBefore = fake->reads;

    SecretReveal reveal{*manager};
    EXPECT_EQ(reveal.state(QStringLiteral("API_KEY")), nullptr);
    EXPECT_EQ(fake->reads, readsBefore) << "nothing is read until the person asks";

    const auto& shown = reveal.toggle(project, QStringLiteral("API_KEY"));
    EXPECT_TRUE(shown.shown);
    EXPECT_EQ(shown.value, QStringLiteral("first-1"));
    EXPECT_EQ(fake->reads, readsBefore + 1);

    // Hide forgets the value; the next show reads what is stored now.
    EXPECT_FALSE(reveal.toggle(project, QStringLiteral("API_KEY")).shown);
    EXPECT_TRUE(reveal.state(QStringLiteral("API_KEY")) == nullptr ||
                reveal.state(QStringLiteral("API_KEY"))->value.isEmpty());
    ASSERT_TRUE(
        manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("second-2"), error));
    EXPECT_EQ(reveal.toggle(project, QStringLiteral("API_KEY")).value, QStringLiteral("second-2"));
}

TEST(SecretReveal, closing_hides_every_value) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-r\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    auto [fake, manager] = makeManager();
    QString error;
    ASSERT_TRUE(
        manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("value-1"), error));
    SecretReveal reveal{*manager};
    ASSERT_TRUE(reveal.toggle(project, QStringLiteral("API_KEY")).shown);

    reveal.hideAll();

    EXPECT_EQ(reveal.state(QStringLiteral("API_KEY")), nullptr);
}

TEST(SecretReveal, a_missing_value_or_a_failed_read_says_so_in_place) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-r\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    auto [fake, manager] = makeManager();
    SecretReveal reveal{*manager};

    const auto notSet = reveal.toggle(project, QStringLiteral("SIGNING_KEY"));
    EXPECT_FALSE(notSet.shown);
    EXPECT_TRUE(notSet.value.isEmpty());
    EXPECT_TRUE(notSet.message.contains(QStringLiteral("No value is stored")))
        << notSet.message.toStdString();

    fake->failReads = true;
    const auto failed = reveal.read(project, QStringLiteral("API_KEY"));
    EXPECT_FALSE(failed.shown);
    EXPECT_TRUE(failed.message.contains(QStringLiteral("locked"))) << failed.message.toStdString();
}

TEST(SecretReveal, a_copy_in_another_folder_claiming_the_id_cannot_reveal) {
    QTemporaryDir original;
    QTemporaryDir copy;
    ASSERT_TRUE(original.isValid());
    ASSERT_TRUE(copy.isValid());
    writeProject(original, "id: shared-r\n");
    writeProject(copy, "id: shared-r\n");
    ProjectModel atOriginal;
    ProjectModel atCopy;
    atOriginal.loadFromDirectory(original.path());
    atCopy.loadFromDirectory(copy.path());
    auto [fake, manager] = makeManager();
    QString error;
    ASSERT_TRUE(
        manager->store(atOriginal, QStringLiteral("API_KEY"), QStringLiteral("original-1"), error));
    const int readsBefore = fake->reads;
    SecretReveal reveal{*manager};

    const auto refused = reveal.toggle(atCopy, QStringLiteral("API_KEY"));

    EXPECT_FALSE(refused.shown);
    EXPECT_TRUE(refused.value.isEmpty());
    EXPECT_FALSE(refused.message.isEmpty());
    EXPECT_EQ(fake->reads, readsBefore) << "the keychain is not read for a conflicting folder";
}

TEST(SecretReveal, a_reveal_stores_nothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeProject(dir, "id: proj-r\n");
    ProjectModel project;
    project.loadFromDirectory(dir.path());
    auto [fake, manager] = makeManager();
    QString error;
    ASSERT_TRUE(
        manager->store(project, QStringLiteral("API_KEY"), QStringLiteral("value-1"), error));
    const int writesBefore = fake->writes;
    // History, the trust file and saved examples all live in the isolated config directory.
    const auto config = ce::configDirectoryForTesting();
    ASSERT_TRUE(config.has_value());
    const auto before = filesUnder(*config);
    const auto projectBefore = filesUnder(dir.path().toStdString());

    SecretReveal reveal{*manager};
    ASSERT_TRUE(reveal.toggle(project, QStringLiteral("API_KEY")).shown);
    reveal.hideAll();

    EXPECT_EQ(fake->writes, writesBefore);
    EXPECT_EQ(filesUnder(*config), before);
    EXPECT_EQ(filesUnder(dir.path().toStdString()), projectBefore);
}
}  // namespace reqloom::desktop::tests

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
