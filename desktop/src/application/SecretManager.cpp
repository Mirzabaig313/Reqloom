// SecretManager — see header. Thin facade over the engine SecretStore.
#include "SecretManager.h"

#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectIdentity.h>
#include <reqloom/engine/ProjectTrustStore.h>

#include <optional>
#include <string>
#include <utility>

namespace reqloom::desktop {

SecretManager::SecretManager(QObject* parent)
    : QObject(parent),
      store_(engine::makeKeychainSecretStore()),
      trust_(engine::makeFileProjectTrustStore()),
      backendAvailable_(engine::keychainBackendAvailable()) {}

SecretManager::SecretManager(std::unique_ptr<engine::SecretStore> store,
                             std::unique_ptr<engine::ProjectTrustStore> trust,
                             QObject* parent)
    : QObject(parent),
      store_(std::move(store)),
      trust_(std::move(trust)),
      backendAvailable_(true) {}

SecretManager::~SecretManager() = default;

bool SecretManager::backendAvailable() const noexcept {
    return backendAvailable_;
}

QList<SecretEntry> SecretManager::referencedSecrets(const ProjectModel& project) const {
    QList<SecretEntry> entries;
    if (!project.hasProject()) {
        return entries;
    }

    // Ask the engine which secrets the project actually references — we only
    // probe the keychain for those names, never enumerate the whole store.
    const auto names = engine::collectSecretReferences(project.project());
    // No keychain access at all unless this folder owns the project id.
    const auto trusted = names.empty()
                             ? std::expected<void, engine::ReqloomError>{}
                             : engine::checkProjectFolder(trust_.get(), project.project());
    for (const auto& name : names) {
        SecretEntry entry;
        entry.name = QString::fromStdString(name);

        if (!trusted) {
            entry.state = SecretState::ReadError;
            entry.detail = QString::fromStdString(trusted.error().detail);
            entries.append(std::move(entry));
            continue;
        }
        const auto key = engine::projectSecretKey(project.project(), name);
        if (!key) {
            entry.state = SecretState::ReadError;
            entry.detail = QString::fromStdString(key.error().detail);
            entries.append(std::move(entry));
            continue;
        }
        auto result = store_->read(*key);
        if (!result) {
            entry.state = SecretState::ReadError;
            entry.detail = QString::fromStdString(result.error().detail);
        } else if (result->has_value()) {
            entry.state = SecretState::Set;
        } else {
            entry.state = SecretState::NotSet;
        }
        entries.append(std::move(entry));
    }
    return entries;
}

namespace {

/// The namespaced store key for `name`, or nullopt with `error` set when the project has no
/// id or this folder does not own it.
std::optional<std::string> keyFor(engine::ProjectTrustStore* trust,
                                  const ProjectModel& project,
                                  const QString& name,
                                  QString& error) {
    if (!project.hasProject()) {
        error = QStringLiteral("No project loaded.");
        return std::nullopt;
    }
    if (auto trusted = engine::checkProjectFolder(trust, project.project()); !trusted) {
        error = QString::fromStdString(trusted.error().detail);
        return std::nullopt;
    }
    auto key = engine::projectSecretKey(project.project(), name.toStdString());
    if (!key) {
        error = QString::fromStdString(key.error().detail);
        return std::nullopt;
    }
    return std::move(*key);
}

}  // namespace

bool SecretManager::store(const ProjectModel& project,
                          const QString& name,
                          const QString& value,
                          QString& error) {
    const auto key = keyFor(trust_.get(), project, name, error);
    if (!key) {
        return false;
    }
    auto result = store_->write(*key, value.toStdString());
    if (!result) {
        error = QString::fromStdString(result.error().detail);
        return false;
    }
    return true;
}

QString SecretManager::folderConflict(const ProjectModel& project) const {
    if (!project.hasProject() || !trust_) {
        return {};
    }
    const auto other = engine::projectFolderConflict(*trust_, project.project());
    return other ? QString::fromStdString(other->string()) : QString{};
}

bool SecretManager::confirmMove(const ProjectModel& project, QString& error) {
    if (!project.hasProject()) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    if (!trust_) {
        error = QStringLiteral("No project trust store is available.");
        return false;
    }
    if (auto bound = engine::rebindProjectFolder(*trust_, project.project()); !bound) {
        error = QString::fromStdString(bound.error().detail);
        return false;
    }
    return true;
}

bool SecretManager::clear(const ProjectModel& project, const QString& name, QString& error) {
    const auto key = keyFor(trust_.get(), project, name, error);
    if (!key) {
        return false;
    }
    auto result = store_->remove(*key);
    if (!result) {
        error = QString::fromStdString(result.error().detail);
        return false;
    }
    return true;
}

}  // namespace reqloom::desktop
