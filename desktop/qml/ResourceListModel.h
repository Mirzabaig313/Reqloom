// ResourceListModel — exposes the loaded project's resources (modules) to QML
// as a list with name + operation count. C++ owns the model; QML owns the view.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>

#include <QtCore/QString>

#include <vector>

namespace reqloom::desktop::qml {

class ResourceListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

public:
    enum Roles : int { NameRole = Qt::UserRole + 1, OperationCountRole };

    explicit ResourceListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace the rows from the given project's resources.
    void reload(const engine::Project& project);

    /// Clear all rows (no project loaded).
    void reset();

private:
    struct Row {
        QString name;
        int operationCount{0};
    };
    std::vector<Row> rows_;
};

}  // namespace reqloom::desktop::qml
