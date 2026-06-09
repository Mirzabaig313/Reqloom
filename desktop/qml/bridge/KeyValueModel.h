// KeyValueModel — a generic two-column (key, value) list model, reused for an
// operation's headers, query params, and extractions in the QML editor.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>

#include <QtCore/QString>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

class KeyValueModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

public:
    enum Roles : int { KeyRole = Qt::UserRole + 1, ValueRole };

    explicit KeyValueModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Replace rows from an ordered string map (insertion order preserved).
    void reload(const std::map<std::string, std::string>& pairs);
    /// Replace rows from explicit key/value pairs (order preserved).
    void reloadPairs(std::vector<std::pair<QString, QString>> pairs);
    void reset();

private:
    std::vector<std::pair<QString, QString>> rows_;
};

}  // namespace reqloom::desktop::qml
