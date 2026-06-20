// ResponseBodyModel — a C++-parsed, virtualized view of a JSON response body
// for the Body (Tree) view. Replaces the per-response JavaScript tree build
// (which re-parsed and flattened the whole payload on every response and
// stuttered on large bodies). The body is parsed once with QJsonDocument, the
// tree is flattened into a pre-order node vector in C++, and only the rows
// whose ancestors are all expanded are exposed to QML — so the ListView stays
// virtualized and the GUI thread never walks the JSON in script.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

#include <cstdint>
#include <vector>

class QJsonValue;

namespace reqloom::desktop::qml {

class ResponseBodyModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

    /// True when the current body parsed as JSON (false → single raw row).
    Q_PROPERTY(bool isJson READ isJson NOTIFY bodyChanged)
    /// Total node count (independent of collapse state), for diagnostics/UX.
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY bodyChanged)

public:
    enum Roles : int {
        DepthRole = Qt::UserRole + 1,  ///< int: indentation depth (0 = top level)
        FieldRole,                     ///< QString: key or array index label
        ValueRole,        ///< QString: display value (containers: {n}/[n]; leaves capped)
        RawValueRole,     ///< QString: full leaf value (uncapped) for copy
        IsLeafRole,       ///< bool: true for scalars/null
        HasChildrenRole,  ///< bool: true for non-empty containers
        CollapsedRole,    ///< bool: this container is collapsed
        PathRole,         ///< QString: JSONPath (e.g. $.data.items[0].id)
    };

    explicit ResponseBodyModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] bool isJson() const noexcept { return isJson_; }
    [[nodiscard]] int nodeCount() const noexcept { return static_cast<int>(nodes_.size()); }

    /// Parse `body` and rebuild the tree. Cheap to call on every response; the
    /// parse + flatten happen once here in C++ rather than in QML per binding.
    void setBody(const QString& body);
    void reset();

    /// Expand/collapse the container at the given visible row. No-op for leaves.
    Q_INVOKABLE void toggle(int visibleRow);
    Q_INVOKABLE void expandAll();
    Q_INVOKABLE void collapseAll();

signals:
    void bodyChanged();

private:
    struct Node {
        int depth{0};
        QString field;
        QString value;     ///< display value (capped for leaves)
        QString rawValue;  ///< full leaf value (for copy)
        QString path;
        bool isLeaf{true};
        bool hasChildren{false};
        bool collapsed{false};
        int subtreeEnd{0};  ///< index one past this node's last descendant
    };

    void walk(const QJsonValue& value, const QString& field, int depth, const QString& path);
    void rebuildVisible();

    std::vector<Node> nodes_;   ///< pre-order flattening of the whole tree
    std::vector<int> visible_;  ///< indices into nodes_ currently shown
    bool isJson_{false};
};

}  // namespace reqloom::desktop::qml
