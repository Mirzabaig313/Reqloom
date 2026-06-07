// ResourceListModel — see header.
#include "ResourceListModel.h"

namespace reqloom::desktop::qml {

int ResourceListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ResourceListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case NameRole:
            return row.name;
        case OperationCountRole:
            return row.operationCount;
        default:
            return {};
    }
}

QHash<int, QByteArray> ResourceListModel::roleNames() const {
    return {{NameRole, "name"}, {OperationCountRole, "operationCount"}};
}

void ResourceListModel::reload(const engine::Project& project) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(project.resources.size());
    for (const auto& [id, resource] : project.resources) {
        rows_.push_back(
            Row{QString::fromStdString(id.value), static_cast<int>(resource.operations.size())});
    }
    endResetModel();
}

void ResourceListModel::reset() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

}  // namespace reqloom::desktop::qml
