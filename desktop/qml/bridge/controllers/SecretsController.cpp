// SecretsController — see header.
#include "SecretsController.h"
#include "AppController.h"

#include "application/ProjectModel.h"

namespace reqloom::desktop::qml {

int SecretListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : entries_.size();
}

QVariant SecretListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_[index.row()];
    switch (role) {
        case NameRole:
            return entry.name;
        case StatusRole:
            switch (entry.state) {
                case SecretState::Set:
                    return QStringLiteral("set");
                case SecretState::NotSet:
                    return QStringLiteral("not set");
                case SecretState::ReadError:
                    return QStringLiteral("error");
            }
            return QStringLiteral("unknown");
        case DetailRole:
            return entry.detail;
        default:
            return {};
    }
}

QHash<int, QByteArray> SecretListModel::roleNames() const {
    return {{NameRole, "name"}, {StatusRole, "status"}, {DetailRole, "detail"}};
}

int SecretListModel::rowForName(const QString& name) const {
    for (int row = 0; row < entries_.size(); ++row) {
        if (entries_[row].name == name) {
            return row;
        }
    }
    return -1;
}

void SecretListModel::reload(const QList<SecretEntry>& entries) {
    beginResetModel();
    entries_ = entries;
    endResetModel();
}

void SecretListModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

SecretsController::SecretsController(QObject* parent) : QObject(parent) {}

SecretsController::~SecretsController() = default;

void SecretsController::refresh() {
    // Reach AppController via its singleton create(); both singletons live on the GUI thread.
    const auto* ctrl = AppController::create(nullptr, nullptr);
    project_ = ctrl->projectRaw();
    reloadList();
}

void SecretsController::reloadList() {
    QString conflict;
    QString folder;
    if (project_ == nullptr || !project_->hasProject()) {
        list_.clear();
    } else {
        conflict = manager_.folderConflict(*project_);
        folder = project_->rootPath();
        list_.reload(manager_.referencedSecrets(*project_));
    }
    if (conflict != folderConflict_) {
        folderConflict_ = conflict;
        emit folderConflictChanged();
    }
    if (folder != currentFolder_) {
        currentFolder_ = folder;
        emit currentFolderChanged();
    }
}

void SecretsController::setLastError(const QString& error) {
    if (error == lastError_) {
        return;
    }
    lastError_ = error;
    emit lastErrorChanged();
}

bool SecretsController::confirmMove() {
    QString error;
    if (project_ == nullptr || !manager_.confirmMove(*project_, error)) {
        setLastError(QStringLiteral("Couldn't use the secrets here: %1")
                         .arg(project_ == nullptr ? QStringLiteral("no project loaded.") : error));
        return false;
    }
    setLastError({});
    reloadList();
    emit notify(QStringLiteral("This folder now owns the project's secrets."), false);
    return true;
}

bool SecretsController::store(const QString& name, const QString& value) {
    QString error;
    if (project_ == nullptr) {
        setLastError(QStringLiteral("Failed to set secret: no project loaded."));
        return false;
    }
    if (manager_.store(*project_, name, value, error)) {
        setLastError({});
        reloadList();
        emit notify(QStringLiteral("Set secret \u201C%1\u201D").arg(name), false);
        return true;
    }
    setLastError(QStringLiteral("Failed to set secret: %1").arg(error));
    return false;
}

bool SecretsController::clear(const QString& name) {
    QString error;
    if (project_ == nullptr) {
        setLastError(QStringLiteral("Failed to clear secret: no project loaded."));
        return false;
    }
    if (manager_.clear(*project_, name, error)) {
        setLastError({});
        reloadList();
        emit notify(QStringLiteral("Cleared secret \u201C%1\u201D").arg(name), false);
        return true;
    }
    setLastError(QStringLiteral("Failed to clear secret: %1").arg(error));
    return false;
}

}  // namespace reqloom::desktop::qml
