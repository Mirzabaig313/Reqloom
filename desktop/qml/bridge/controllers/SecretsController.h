// SecretsController — QML-facing bridge over SecretManager. Exposes the list
// of required {{secret.NAME}} entries + their keychain state, and lets QML
// set / clear individual secrets. Never returns stored values to QML.
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

public:
    explicit SecretsController(QObject* parent = nullptr);
    ~SecretsController() override;

    SecretsController(const SecretsController&) = delete;
    SecretsController& operator=(const SecretsController&) = delete;
    SecretsController(SecretsController&&) = delete;
    SecretsController& operator=(SecretsController&&) = delete;

    static SecretsController* create(QQmlEngine*, QJSEngine*) {
        static SecretsController instance;
        // CppOwnership: don't let the QML engine delete this static singleton
        // at teardown (would be a free of non-heap memory).
        QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
        return &instance;
    }

    [[nodiscard]] SecretListModel* secrets() { return &list_; }
    [[nodiscard]] bool backendAvailable() const noexcept { return manager_.backendAvailable(); }

    /// Refresh the list against `project`. Call when a project loads or the
    /// dialog opens.
    Q_INVOKABLE void refresh();
    /// Store/update a secret value. Emits notify on result.
    Q_INVOKABLE bool store(const QString& name, const QString& value);
    /// Clear a secret. Emits notify on result.
    Q_INVOKABLE bool clear(const QString& name);

signals:
    void notify(QString message, bool isError);

private:
    SecretManager manager_;
    SecretListModel list_;
    const ProjectModel* project_{nullptr};
};

}  // namespace reqloom::desktop::qml
