// ChainEditorModel — see header. Owns per-operation dependency + extraction
// editors for the whole-chain editor.
#include "ChainEditorModel.h"

namespace reqloom::desktop::qml {

ChainEditorModel::ChainEditorModel(QObject* parent) : QAbstractListModel(parent) {}

int ChainEditorModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant ChainEditorModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case OperationIdRole:
            return row.operationId;
        case MethodRole:
            return row.method;
        case IsTargetRole:
            return row.isTarget;
        case DepModelRole:
            return QVariant::fromValue(static_cast<QObject*>(row.deps.get()));
        case ExtractModelRole:
            return QVariant::fromValue(static_cast<QObject*>(row.extracts.get()));
        case CandidatesRole:
            return row.candidates;
        case ForEachOverRole:
            return row.forEachOver;
        case ForEachContinueOnErrorRole:
            return row.forEachContinueOnError;
        default:
            return {};
    }
}

QHash<int, QByteArray> ChainEditorModel::roleNames() const {
    return {
        {OperationIdRole, "operationId"},
        {MethodRole, "method"},
        {IsTargetRole, "isTarget"},
        {DepModelRole, "depModel"},
        {ExtractModelRole, "extractModel"},
        {CandidatesRole, "candidates"},
        {ForEachOverRole, "forEachOver"},
        {ForEachContinueOnErrorRole, "forEachContinueOnError"},
    };
}

void ChainEditorModel::rebuild(const std::vector<OpSeed>& seeds) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(seeds.size());
    for (const OpSeed& seed : seeds) {
        Row row;
        row.operationId = seed.operationId;
        row.method = seed.method;
        row.isTarget = seed.isTarget;
        row.candidates = seed.candidates;
        row.forEachOver = seed.forEachOver;
        row.forEachContinueOnError = seed.forEachContinueOnError;
        // Per-row models are parented to this model (C++ ownership), so QML
        // never deletes them when a delegate is destroyed.
        row.deps = std::make_unique<DependencyEditModel>(this);
        row.deps->setCandidates(seed.candidates);
        row.deps->setDependencies(seed.dependencies);
        row.extracts = std::make_unique<EditableKeyValueModel>(this);
        row.extracts->setPairs(seed.extractions);
        const auto forwardEditorChange = [this]() {
            emit editorChanged();
        };
        for (QAbstractItemModel* editor : {static_cast<QAbstractItemModel*>(row.deps.get()),
                                           static_cast<QAbstractItemModel*>(row.extracts.get())}) {
            connect(editor, &QAbstractItemModel::dataChanged, this, forwardEditorChange);
            connect(editor, &QAbstractItemModel::rowsInserted, this, forwardEditorChange);
            connect(editor, &QAbstractItemModel::rowsRemoved, this, forwardEditorChange);
            connect(editor, &QAbstractItemModel::modelReset, this, forwardEditorChange);
        }
        rows_.push_back(std::move(row));
    }
    endResetModel();
    emit editorChanged();
}

std::vector<ChainEditorModel::OpSeed> ChainEditorModel::snapshotSeeds() const {
    std::vector<OpSeed> seeds;
    seeds.reserve(rows_.size());
    for (const Row& row : rows_) {
        OpSeed seed;
        seed.operationId = row.operationId;
        seed.method = row.method;
        seed.isTarget = row.isTarget;
        seed.candidates = row.candidates;
        seed.forEachOver = row.forEachOver;
        seed.forEachContinueOnError = row.forEachContinueOnError;
        if (row.deps) {
            seed.dependencies = row.deps->dependencies();
        }
        if (row.extracts) {
            seed.extractions = row.extracts->pairs();
        }
        seeds.push_back(std::move(seed));
    }
    return seeds;
}

QString ChainEditorModel::operationIdAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    return rows_[static_cast<std::size_t>(row)].operationId;
}

DependencyEditModel* ChainEditorModel::depModelAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return rows_[static_cast<std::size_t>(row)].deps.get();
}

EditableKeyValueModel* ChainEditorModel::extractModelAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return rows_[static_cast<std::size_t>(row)].extracts.get();
}

QString ChainEditorModel::forEachOverAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return {};
    }
    return rows_[static_cast<std::size_t>(row)].forEachOver;
}

void ChainEditorModel::setForEachOver(int row, const QString& overResource) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    Row& target = rows_[static_cast<std::size_t>(row)];
    if (target.forEachOver == overResource) {
        return;
    }
    target.forEachOver = overResource;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ForEachOverRole});
    emit editorChanged();
}

bool ChainEditorModel::forEachContinueOnErrorAt(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return false;
    }
    return rows_[static_cast<std::size_t>(row)].forEachContinueOnError;
}

void ChainEditorModel::setForEachContinueOnError(int row, bool continueOnError) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    Row& target = rows_[static_cast<std::size_t>(row)];
    if (target.forEachContinueOnError == continueOnError) {
        return;
    }
    target.forEachContinueOnError = continueOnError;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ForEachContinueOnErrorRole});
    emit editorChanged();
}
}  // namespace reqloom::desktop::qml
