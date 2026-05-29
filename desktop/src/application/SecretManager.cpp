// SecretManager — see header. Thin facade over the engine SecretStore.
#include "SecretManager.h"

#include "ProjectModel.h"

#include <chainapi/engine/Factories.h>

#include <utility>

namespace chainapi::desktop {

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

        auto result = store_->read(name);
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

bool SecretManager::store(const QString& name, const QString& value, QString& error) {
    auto result = store_->write(name.toStdString(), value.toStdString());
    if (!result) {
        error = QString::fromStdString(result.error().detail);
        return false;
    }
    return true;
}

bool SecretManager::clear(const QString& name, QString& error) {
    auto result = store_->remove(name.toStdString());
    if (!result) {
        error = QString::fromStdString(result.error().detail);
        return false;
    }
    return true;
}

}  // namespace chainapi::desktop
