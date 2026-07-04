// AuthStepListModel — editable list of an actor's auth-chain login steps. Each
// row is one HTTP request (method, path, body, expect) plus its own extractions
// (a nested EditableKeyValueModel), mirroring the engine's Actor::authSteps
// (std::vector<AuthStep>). C++ owns the per-row extraction models; QML edits
// rows via the invokables. Backs the N-step login editor in ActorDetail.
#pragma once

#include "EditableKeyValueModel.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>

#include <memory>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

class AuthStepListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

public:
    enum Roles : int {
        StepIdRole = Qt::UserRole + 1,  ///< QString stable id (kept across edits)
        MethodRole,                     ///< QString HTTP method label
        PathRole,                       ///< QString path template
        BodyRole,                       ///< QString body template
        ExpectRole,                     ///< QString expected status ("" = any 2xx)
        ExtractModelRole,               ///< EditableKeyValueModel* (QObject*)
    };

    /// Seed for one login step.
    struct StepSeed {
        QString id;
        QString method;
        QString path;
        QString body;
        QString expect;
        std::vector<std::pair<QString, QString>> extractions;
    };

    explicit AuthStepListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace all rows with fresh per-step editors seeded from `seeds`.
    void rebuild(const std::vector<StepSeed>& seeds);

    // ── QML-facing mutation (edit mode) ──
    Q_INVOKABLE void setMethod(int row, const QString& method);
    Q_INVOKABLE void setPath(int row, const QString& path);
    Q_INVOKABLE void setBody(int row, const QString& body);
    Q_INVOKABLE void setExpect(int row, const QString& expect);
    /// Append a blank step (POST, empty path). Returns the new row index.
    Q_INVOKABLE int addStep();
    /// Remove `row`. Refuses to remove the last remaining step — a step-based
    /// actor needs at least one login request.
    Q_INVOKABLE void removeStep(int row);
    /// Move `row` up (delta -1) or down (delta +1); a no-op at the ends.
    Q_INVOKABLE void moveStep(int row, int delta);

    // ── Read-back for the save path ──
    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }
    [[nodiscard]] QString methodAt(int row) const;
    [[nodiscard]] QString pathAt(int row) const;
    [[nodiscard]] QString bodyAt(int row) const;
    [[nodiscard]] QString expectAt(int row) const;
    [[nodiscard]] QString idAt(int row) const;
    [[nodiscard]] EditableKeyValueModel* extractModelAt(int row) const;

private:
    struct Row {
        QString id;
        QString method;
        QString path;
        QString body;
        QString expect;
        std::unique_ptr<EditableKeyValueModel> extracts;
    };

    [[nodiscard]] bool valid(int row) const {
        return row >= 0 && row < static_cast<int>(rows_.size());
    }

    std::vector<Row> rows_;
};

}  // namespace reqloom::desktop::qml
