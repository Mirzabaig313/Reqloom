// EditableKeyValueModel — an editable two-column (key, value) list with an
// always-present trailing blank "ghost" row (Apidog pattern), per QML Migration
// Roadmap WS-A. Used for the New Endpoint dialog's extraction table (variable +
// path). C++ owns the row state; QML edits it via the invokables.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

class EditableKeyValueModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by AppController")

public:
    enum Roles : int { KeyRole = Qt::UserRole + 1, ValueRole, IsGhostRole };

    explicit EditableKeyValueModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Edit the key of `row`; appends a fresh ghost row when the last row is
    /// first filled, and drops a now-empty non-last row.
    Q_INVOKABLE void setKey(int row, const QString& key);
    /// Edit the value of `row` (same ghost-row maintenance as setKey).
    Q_INVOKABLE void setValue(int row, const QString& value);
    /// Remove `row` outright (the trailing ghost row is never removable).
    Q_INVOKABLE void removeRow(int row);

    /// Replace all rows with `pairs`, then re-append the trailing ghost row.
    void setPairs(std::vector<std::pair<QString, QString>> pairs);
    /// Reset to a single blank ghost row.
    void clearRows();

    /// Current rows excluding blanks (both key and value empty are dropped;
    /// the caller decides whether an empty key/value is meaningful).
    [[nodiscard]] std::vector<std::pair<QString, QString>> pairs() const;

private:
    void ensureTrailingGhost();
    [[nodiscard]] bool rowIsBlank(std::size_t row) const;

    std::vector<std::pair<QString, QString>> rows_;
};

}  // namespace reqloom::desktop::qml
