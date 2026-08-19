// ExampleListModel — saved example responses for the currently-open operation
//  A thin list view over SavedResponseStore::list
// for one operation id; AppController refills it on selection + after any
// example mutation. C++ owns the data; QML renders the rows.
#pragma once

#include "application/SavedResponseStore.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QList>

namespace reqloom::desktop::qml {

class ExampleListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

public:
    enum Roles : int {
        NameRole = Qt::UserRole + 1,  ///< QString: example name
        StatusRole,                   ///< int: stored HTTP status code
    };

    explicit ExampleListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace the rows with the examples for the current operation.
    void setExamples(const QList<SavedResponse>& examples);
    void clear();

private:
    QList<SavedResponse> examples_;
};

}  // namespace reqloom::desktop::qml
