// SecretsController — see header.
#include "SecretsController.h"
#include "AppController.h"

#include "application/ProjectModel.h"

namespace reqloom::desktop::qml {

// ── SecretListModel ─────────────────────────────────────────────────────────

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

// ── SecretsController ────────────────────────────────────────────────────────

SecretsController::SecretsController(QObject* parent) : QObject(parent) {}

SecretsController::~SecretsController() = default;

void SecretsController::refresh() {
    // Reach AppController through its singleton create() — safe on the GUI
    // thread; both singletons are created in the same thread.
    const auto* ctrl = AppController::create(nullptr, nullptr);
    project_ = ctrl->projectRaw();
    if (project_ == nullptr || !project_->hasProject()) {
        list_.clear();
        return;
    }
    list_.reload(manager_.referencedSecrets(*project_));
}

bool SecretsController::store(const QString& name, const QString& value) {
    QString error;
    if (manager_.store(name, value, error)) {
        if (project_ != nullptr) {
            list_.reload(manager_.referencedSecrets(*project_));
        }
        emit notify(QStringLiteral("Set secret \u201C%1\u201D").arg(name), false);
        return true;
    }
    emit notify(QStringLiteral("Failed to set secret: %1").arg(error), true);
    return false;
}

bool SecretsController::clear(const QString& name) {
    QString error;
    if (manager_.clear(name, error)) {
        if (project_ != nullptr) {
            list_.reload(manager_.referencedSecrets(*project_));
        }
        emit notify(QStringLiteral("Cleared secret \u201C%1\u201D").arg(name), false);
        return true;
    }
    emit notify(QStringLiteral("Failed to clear secret: %1").arg(error), true);
    return false;
}

}  // namespace reqloom::desktop::qml
