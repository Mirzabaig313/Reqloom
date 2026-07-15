// DependencyEditModel — editable depends_on picker list
//  Each row is a selection among existing operation ids, so a dependency
// can never name something undefined. An always-present trailing blank row
// grows the list (Apidog ghost-row pattern). C++ owns state; QML renders combos.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <string>
#include <vector>

namespace reqloom::desktop::qml {

class DependencyEditModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by AppController")

    Q_PROPERTY(QStringList candidates READ candidates NOTIFY candidatesChanged)

public:
    enum Roles : int { ValueRole = Qt::UserRole + 1, IsGhostRole };

    explicit DependencyEditModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QStringList candidates() const { return candidates_; }

    /// Replace the pickable operation ids and reset rows to a single blank one.
    void setCandidates(const QStringList& operationIds);

    /// Seed the picker rows from an existing operation's dependencies, then
    /// append the trailing blank ghost row. Used when opening an op for edit.
    void setDependencies(const std::vector<std::string>& dependencies);

    /// Set the selection of `row` (empty string clears it). Appends a fresh
    /// blank row when the last row is first filled.
    Q_INVOKABLE void setSelection(int row, const QString& value);
    /// Remove `row` (the trailing blank row is never removable).
    Q_INVOKABLE void removeRow(int row);

    /// Current picks in row order, de-duplicated, blanks dropped.
    [[nodiscard]] std::vector<std::string> dependencies() const;

signals:
    void candidatesChanged();

private:
    void ensureTrailingGhost();

    QStringList candidates_;
    std::vector<QString> rows_;
};

}  // namespace reqloom::desktop::qml
