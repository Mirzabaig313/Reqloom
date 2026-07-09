// ChainEditorModel — backs the whole-chain editor in the Chain tab. Each row is
// one operation in the target's transitive dependency chain, owning its own
// editable depends_on (DependencyEditModel) and extract (EditableKeyValueModel)
// so every step can be wired from one place without hand-editing YAML. C++ owns
// the per-row models; QML binds DependencyEditor/ExtractionEditor to them.
#pragma once

#include "DependencyEditModel.h"
#include "EditableKeyValueModel.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

class ChainEditorModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

public:
    enum Roles : int {
        OperationIdRole = Qt::UserRole + 1,  ///< QString "<resource>.<op>"
        MethodRole,                          ///< QString HTTP method label
        IsTargetRole,                        ///< bool: the operation being edited
        DepModelRole,                        ///< DependencyEditModel* (QObject*)
        ExtractModelRole,                    ///< EditableKeyValueModel* (QObject*)
        CandidatesRole,                      ///< QStringList of pickable op ids
        ForEachOverRole,                     ///< QString resource to iterate ("" = run once)
        ForEachContinueOnErrorRole,          ///< bool: keep going after a failed iteration
    };

    /// Seed for one operation row.
    struct OpSeed {
        QString operationId;
        QString method;
        bool isTarget{false};
        std::vector<std::string> dependencies;
        std::vector<std::pair<QString, QString>> extractions;
        QStringList candidates;
        QString forEachOver;  ///< resource id to fan out over, empty = run once
        bool forEachContinueOnError{false};
    };

    explicit ChainEditorModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace all rows with fresh per-operation editors seeded from `seeds`.
    void rebuild(const std::vector<OpSeed>& seeds);

    /// Capture the current rows (including live depends_on / extract / for-each
    /// edits) as seeds, so a caller can park the chain and rebuild() it later.
    [[nodiscard]] std::vector<OpSeed> snapshotSeeds() const;

    // Read-back accessors for the save path.
    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }
    [[nodiscard]] QString operationIdAt(int row) const;
    [[nodiscard]] DependencyEditModel* depModelAt(int row) const;
    [[nodiscard]] EditableKeyValueModel* extractModelAt(int row) const;

    /// Read/patch the per-step for-each target resource ("" = run once).
    [[nodiscard]] QString forEachOverAt(int row) const;
    void setForEachOver(int row, const QString& overResource);

    /// Read/patch the per-step for-each continue-on-error flag.
    [[nodiscard]] bool forEachContinueOnErrorAt(int row) const;
    void setForEachContinueOnError(int row, bool continueOnError);

private:
    struct Row {
        QString operationId;
        QString method;
        bool isTarget{false};
        QStringList candidates;
        QString forEachOver;
        bool forEachContinueOnError{false};
        std::unique_ptr<DependencyEditModel> deps;
        std::unique_ptr<EditableKeyValueModel> extracts;
    };
    std::vector<Row> rows_;
};

}  // namespace reqloom::desktop::qml
