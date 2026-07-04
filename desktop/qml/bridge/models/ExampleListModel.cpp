// ExampleListModel — see header.
#include "ExampleListModel.h"

namespace reqloom::desktop::qml {

int ExampleListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(examples_.size());
}

QVariant ExampleListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= examples_.size()) {
        return {};
    }
    const SavedResponse& example = examples_.at(index.row());
    switch (role) {
        case NameRole:
            return example.name;
        case StatusRole:
            return example.status;
        default:
            return {};
    }
}

QHash<int, QByteArray> ExampleListModel::roleNames() const {
    return {{NameRole, "name"}, {StatusRole, "status"}};
}

void ExampleListModel::setExamples(const QList<SavedResponse>& examples) {
    beginResetModel();
    examples_ = examples;
    endResetModel();
}

void ExampleListModel::clear() {
    beginResetModel();
    examples_.clear();
    endResetModel();
}

}  // namespace reqloom::desktop::qml
