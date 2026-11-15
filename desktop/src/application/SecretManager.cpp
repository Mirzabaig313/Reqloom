// SecretManager — see header. Thin facade over the engine SecretStore.
#include "SecretManager.h"

#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectIdentity.h>

#include <optional>
#include <string>
#include <utility>

namespace reqloom::desktop {

SecretManager::SecretManager(QObject* parent)
    : QObject(parent),
      store_(engine::makeKeychainSecretStore()),
      backendAvailable_(engine::keychainBackendAvailable()) {}

SecretManager::SecretManager(std::unique_ptr<engine::SecretStore> store, QObject* parent)
    : QObject(parent), store_(std::move(store)), backendAvailable_(true) {}

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
    for (const auto& name : engine::collectSecretReferences(project.project())) {
        SecretEntry entry;
        entry.name = QString::fromStdString(name);

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

/// The namespaced store key for `name` in the open project, or nullopt with
/// `error` set when there is no project or it has no id.
std::optional<std::string> keyFor(const ProjectModel& project,
                                  const QString& name,
                                  QString& error) {
    if (!project.hasProject()) {
        error = QStringLiteral("No project loaded.");
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
    const auto key = keyFor(project, name, error);
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

bool SecretManager::clear(const ProjectModel& project, const QString& name, QString& error) {
    const auto key = keyFor(project, name, error);
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
