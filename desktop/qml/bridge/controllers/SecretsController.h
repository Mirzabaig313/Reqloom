// SecretsController — QML bridge over SecretManager: lists required {{secret.NAME}} entries with
// their keychain state and sets/clears secrets. Never returns stored values.
#pragma once

#include "application/SecretManager.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtQml/QQmlEngine>

#include <memory>

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop {
class ProjectModel;
}  // namespace reqloom::desktop

namespace reqloom::desktop::qml {

/// One row in the secrets list: name + status string.
class SecretListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles : int { NameRole = Qt::UserRole + 1, StatusRole, DetailRole };

    explicit SecretListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE [[nodiscard]] int rowForName(const QString& name) const;

    void reload(const QList<SecretEntry>& entries);
    void clear();

private:
    QList<SecretEntry> entries_;
};

class SecretsController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(SecretListModel* secrets READ secrets CONSTANT)
    Q_PROPERTY(bool backendAvailable READ backendAvailable CONSTANT)
    /// The folder this machine records for the open project's id when the
    /// project now lives elsewhere (moved or copied). Empty = no conflict.
    Q_PROPERTY(QString folderConflict READ folderConflict NOTIFY folderConflictChanged)
    /// The folder the open project is loaded from. Empty when none is loaded.
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY currentFolderChanged)
    /// Why the last store/clear/confirmMove failed; empty after a success.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit SecretsController(QObject* parent = nullptr);
    ~SecretsController() override;

    SecretsController(const SecretsController&) = delete;
    SecretsController& operator=(const SecretsController&) = delete;
    SecretsController(SecretsController&&) = delete;
    SecretsController& operator=(SecretsController&&) = delete;

    static SecretsController* create(QQmlEngine*, QJSEngine*) {
        static SecretsController instance;
        // CppOwnership: the QML engine must not delete this static singleton at teardown.
        QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
        return &instance;
    }

    [[nodiscard]] SecretListModel* secrets() { return &list_; }
    [[nodiscard]] bool backendAvailable() const noexcept { return manager_.backendAvailable(); }
    [[nodiscard]] QString folderConflict() const { return folderConflict_; }
    [[nodiscard]] QString currentFolder() const { return currentFolder_; }
    [[nodiscard]] QString lastError() const { return lastError_; }

    /// Refresh the list against `project`. Call when a project loads or the
    /// dialog opens.
    Q_INVOKABLE void refresh();
    /// Store/update a secret value. Emits notify on success; a failure sets lastError.
    Q_INVOKABLE bool store(const QString& name, const QString& value);
    /// Clear a secret. Emits notify on success; a failure sets lastError.
    Q_INVOKABLE bool clear(const QString& name);
    /// The user confirms the open project was moved here: bind its id to
    /// this folder so its secrets work here. `shownOwner` is the recorded folder
    /// the user was shown; the move is refused if the record changed. Emits
    /// notify on success; a failure sets lastError and refreshes folderConflict.
    Q_INVOKABLE bool confirmMove(const QString& shownOwner);

signals:
    void notify(QString message, bool isError);
    void folderConflictChanged();
    void currentFolderChanged();
    void lastErrorChanged();

private:
    SecretManager manager_;
    SecretListModel list_;
    const ProjectModel* project_{nullptr};
    QString folderConflict_;
    QString currentFolder_;
    QString lastError_;

    void reloadList();
    void setLastError(const QString& error);
};

}  // namespace reqloom::desktop::qml
