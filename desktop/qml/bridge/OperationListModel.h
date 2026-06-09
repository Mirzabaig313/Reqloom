// OperationListModel — exposes one module's operations (endpoints) to QML:
// HTTP method, short name, and path template. C++ owns the model; QML renders.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>

#include <QtCore/QString>

#include <vector>

namespace reqloom::desktop::qml {

class OperationListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

public:
    enum Roles : int { MethodRole = Qt::UserRole + 1, NameRole, PathRole };

    explicit OperationListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace rows with the operations of `resource`.
    void reload(const engine::Resource& resource);
    void reset();

private:
    struct Row {
        QString method;
        QString name;
        QString path;
    };
    std::vector<Row> rows_;
};

}  // namespace reqloom::desktop::qml
