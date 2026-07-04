// AuthStepListModel — see header. Owns the per-step extraction editors for the
// actor's N-step login chain.
#include "AuthStepListModel.h"

#include <utility>

namespace reqloom::desktop::qml {

AuthStepListModel::AuthStepListModel(QObject* parent) : QAbstractListModel(parent) {}

int AuthStepListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AuthStepListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case StepIdRole:
            return row.id;
        case MethodRole:
            return row.method;
        case PathRole:
            return row.path;
        case BodyRole:
            return row.body;
        case ExpectRole:
            return row.expect;
        case ExtractModelRole:
            return QVariant::fromValue(static_cast<QObject*>(row.extracts.get()));
        default:
            return {};
    }
}

QHash<int, QByteArray> AuthStepListModel::roleNames() const {
    return {
        {StepIdRole, "stepId"},
        {MethodRole, "method"},
        {PathRole, "path"},
        {BodyRole, "body"},
        {ExpectRole, "expect"},
        {ExtractModelRole, "extractModel"},
    };
}

void AuthStepListModel::rebuild(const std::vector<StepSeed>& seeds) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(seeds.size());
    for (const StepSeed& seed : seeds) {
        Row row;
        row.id = seed.id;
        row.method = seed.method;
        row.path = seed.path;
        row.body = seed.body;
        row.expect = seed.expect;
        // Parented to this model (C++ ownership) so QML never deletes it when a
        // delegate is destroyed.
        row.extracts = std::make_unique<EditableKeyValueModel>(this);
        row.extracts->setPairs(seed.extractions);
        rows_.push_back(std::move(row));
    }
    endResetModel();
}

void AuthStepListModel::setMethod(int row, const QString& method) {
    if (!valid(row) || rows_[static_cast<std::size_t>(row)].method == method) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].method = method;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {MethodRole});
}

void AuthStepListModel::setPath(int row, const QString& path) {
    if (!valid(row) || rows_[static_cast<std::size_t>(row)].path == path) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].path = path;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {PathRole});
}

void AuthStepListModel::setBody(int row, const QString& body) {
    if (!valid(row) || rows_[static_cast<std::size_t>(row)].body == body) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].body = body;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {BodyRole});
}

void AuthStepListModel::setExpect(int row, const QString& expect) {
    if (!valid(row) || rows_[static_cast<std::size_t>(row)].expect == expect) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].expect = expect;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ExpectRole});
}

int AuthStepListModel::addStep() {
    const int at = static_cast<int>(rows_.size());
    beginInsertRows({}, at, at);
    Row row;
    row.method = QStringLiteral("POST");
    row.extracts = std::make_unique<EditableKeyValueModel>(this);
    rows_.push_back(std::move(row));
    endInsertRows();
    return at;
}

void AuthStepListModel::removeStep(int row) {
    // A step-based actor must retain at least one login request.
    if (!valid(row) || rows_.size() <= 1) {
        return;
    }
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
}

void AuthStepListModel::moveStep(int row, int delta) {
    const int target = row + delta;
    // Adjacent single-row moves only (delta ±1 from the up/down buttons).
    // beginMoveRows physically relocates the delegate so a step whose text
    // fields the user already edited keeps its (binding-broken) widget state on
    // reorder — a plain swap + dataChanged cannot push values back into a
    // focused TextField/ComboBox.
    if ((delta != 1 && delta != -1) || !valid(row) || !valid(target)) {
        return;
    }
    // Qt expresses the destination as the index the row lands *before*; moving
    // down by one therefore needs target + 1.
    const int destination = delta > 0 ? target + 1 : target;
    beginMoveRows({}, row, row, {}, destination);
    std::swap(rows_[static_cast<std::size_t>(row)], rows_[static_cast<std::size_t>(target)]);
    endMoveRows();
}

QString AuthStepListModel::methodAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].method : QString{};
}

QString AuthStepListModel::pathAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].path : QString{};
}

QString AuthStepListModel::bodyAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].body : QString{};
}

QString AuthStepListModel::expectAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].expect : QString{};
}

QString AuthStepListModel::idAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].id : QString{};
}

EditableKeyValueModel* AuthStepListModel::extractModelAt(int row) const {
    return valid(row) ? rows_[static_cast<std::size_t>(row)].extracts.get() : nullptr;
}

}  // namespace reqloom::desktop::qml
