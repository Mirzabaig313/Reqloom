// OperationListModel — see header.
#include "OperationListModel.h"

#include <array>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] QString methodLabel(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

}  // namespace

int OperationListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant OperationListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case MethodRole:
            return row.method;
        case NameRole:
            return row.name;
        case PathRole:
            return row.path;
        default:
            return {};
    }
}

QHash<int, QByteArray> OperationListModel::roleNames() const {
    return {{MethodRole, "method"}, {NameRole, "name"}, {PathRole, "path"}};
}

void OperationListModel::reload(const engine::Resource& resource) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(resource.operations.size());
    for (const auto& [opName, op] : resource.operations) {
        rows_.push_back(Row{methodLabel(op.method),
                            QString::fromStdString(opName),
                            QString::fromStdString(op.pathTemplate)});
    }
    endResetModel();
}

void OperationListModel::reset() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

}  // namespace reqloom::desktop::qml
