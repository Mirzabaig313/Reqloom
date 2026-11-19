// SecretsController — QML bridge over SecretManager: lists required {{secret.NAME}} entries with
// their keychain state and sets/clears secrets. A stored value reaches the UI only when the person
// asks to see it (`toggleReveal`, `readStoredValue`), read from the keychain at that moment; it is
// held only while shown and is never written, logged or copied.
#pragma once

#include "application/SecretManager.h"
#include "application/SecretReveal.h"

#include <reqloom/engine/HostApproval.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>
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
    enum Roles : int {
        NameRole = Qt::UserRole + 1,
        StatusRole,
        DetailRole,
        RevealedRole,       ///< bool: the stored value is shown
        RevealedValueRole,  ///< QString: the value while shown, else empty
        RevealMessageRole,  ///< QString: why nothing is shown (not set, read failed)
    };

    explicit SecretListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    /// The reveal state rows read; not owned.
    void setReveal(const SecretReveal* reveal) { reveal_ = reveal; }
    /// Repaint `name`'s reveal roles.
    void revealChanged(const QString& name);
    /// Repaint every row's reveal roles.
    void allRevealsChanged();

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE [[nodiscard]] int rowForName(const QString& name) const;

    void reload(const QList<SecretEntry>& entries);
    void clear();

private:
    QList<SecretEntry> entries_;
    const SecretReveal* reveal_{nullptr};
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
    /// Hosts this user approved to receive the open project's credentials (FR-15.9), sorted.
    Q_PROPERTY(QStringList approvedHosts READ approvedHosts NOTIFY approvedHostsChanged)
    /// The user setting: may the reqloom command line approve hosts? Off leaves this app as the
    /// only place to approve one.
    Q_PROPERTY(bool cliHostApproval READ cliHostApproval WRITE setCliHostApproval NOTIFY
                   cliHostApprovalChanged)

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
    [[nodiscard]] QStringList approvedHosts() const { return approvedHosts_; }
    [[nodiscard]] bool cliHostApproval() const;
    void setCliHostApproval(bool allowed);

    /// Forget an approved host for the open project. A failure sets lastError.
    Q_INVOKABLE bool revokeHost(const QString& host);

    /// Refresh the list against `project`. Call when a project loads or the
    /// dialog opens.
    Q_INVOKABLE void refresh();
    /// Store/update a secret value. Emits notify on success; a failure sets lastError.
    Q_INVOKABLE bool store(const QString& name, const QString& value);
    /// Clear a secret. Emits notify on success; a failure sets lastError.
    Q_INVOKABLE bool clear(const QString& name);
    /// Show `name`'s stored value (read from the keychain now) or, when shown, hide it. The
    /// row's `revealed`, `revealedValue` and `revealMessage` roles carry the result.
    Q_INVOKABLE void toggleReveal(const QString& name);
    /// Hide and forget every shown value (the dialog closes).
    Q_INVOKABLE void hideRevealed();
    /// The value stored for `name` now, for a one-off view in the request panel; nothing is kept
    /// here. `{ shown: bool, value: string, message: string }`.
    Q_INVOKABLE QVariantMap readStoredValue(const QString& name) const;

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
    void approvedHostsChanged();
    void cliHostApprovalChanged();

private:
    SecretManager manager_;
    SecretReveal reveal_{manager_};
    SecretListModel list_;
    const ProjectModel* project_{nullptr};
    QString folderConflict_;
    QString currentFolder_;
    QString lastError_;
    QStringList approvedHosts_;
    std::unique_ptr<engine::HostApprovalStore> hostApprovals_;

    void reloadList();
    void setLastError(const QString& error);
};

}  // namespace reqloom::desktop::qml
