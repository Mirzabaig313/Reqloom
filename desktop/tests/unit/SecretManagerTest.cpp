// Tests SecretManager against an in-memory SecretStore (no real keychain).
// Confirms it reports per-secret presence for a project's {{secret.NAME}}
// references, and that store/clear round-trip through the injected store.
#include "application/ProjectModel.h"
#include "application/SecretManager.h"

#include <chainapi/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace chainapi::desktop::tests {

namespace {

namespace ce = chainapi::engine;

/// In-memory SecretStore standing in for the OS keychain.
class FakeSecretStore final : public ce::SecretStore {
public:
    std::expected<std::optional<std::string>, ce::ChainApiError> read(
        const std::string& name) override {
        if (auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }

    std::expected<void, ce::ChainApiError> write(const std::string& name,
                                                 const std::string& value) override {
        values_[name] = value;
        return {};
    }

    std::expected<void, ce::ChainApiError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

    [[nodiscard]] bool has(const std::string& name) const { return values_.contains(name); }

private:
    std::map<std::string, std::string> values_;
};

/// Build a SecretManager wrapping a FakeSecretStore we retain a handle to.
struct ManagerWithFake {
    FakeSecretStore* fake{nullptr};
    std::unique_ptr<SecretManager> manager;
};

[[nodiscard]] ManagerWithFake makeManager() {
    auto store = std::make_unique<FakeSecretStore>();
    auto* raw = store.get();
    return ManagerWithFake{raw, std::make_unique<SecretManager>(std::move(store), nullptr)};
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
void writeProject(const QTemporaryDir& dir) {
    const QString path = QDir(dir.path()).filePath(QStringLiteral("chainapi.yaml"));
    std::ofstream out(path.toStdString());
    out << kProjectYaml;
}

}  // namespace

TEST(SecretManager, store_then_clear_round_trips_through_backend) {
    auto [fake, manager] = makeManager();

    QString error;
    ASSERT_TRUE(manager->store(QStringLiteral("API_KEY"), QStringLiteral("s3cr3t"), error))
        << error.toStdString();
    EXPECT_TRUE(fake->has("API_KEY"));

    ASSERT_TRUE(manager->clear(QStringLiteral("API_KEY"), error)) << error.toStdString();
    EXPECT_FALSE(fake->has("API_KEY"));
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
    ASSERT_TRUE(manager->store(QStringLiteral("API_KEY"), QStringLiteral("v"), error))
        << error.toStdString();

    auto after = manager->referencedSecrets(project);
    ASSERT_EQ(after.size(), 2);
    EXPECT_EQ(after[0].name, QStringLiteral("API_KEY"));
    EXPECT_EQ(after[0].state, SecretState::Set);
    EXPECT_EQ(after[1].name, QStringLiteral("SIGNING_KEY"));
    EXPECT_EQ(after[1].state, SecretState::NotSet);
}

}  // namespace chainapi::desktop::tests

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
