// ResponseBodyModel — see header. Parses the body with QJsonDocument and
// flattens it pre-order into `nodes_`; `visible_` projects the rows whose
// ancestors are all expanded.
#include "ResponseBodyModel.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace reqloom::desktop::qml {

namespace {

// Cap a leaf's display string so a multi-megabyte scalar (e.g. an embedded
// base64 blob) can't create a giant Text item. The full value stays available
// via RawValueRole for copy.
constexpr int kMaxDisplayValue = 512;

[[nodiscard]] QString scalarToString(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double: {
            const double d = value.toDouble();
            // Render integers without a trailing ".0"; keep precision otherwise.
            if (d == static_cast<double>(static_cast<qint64>(d))) {
                return QString::number(static_cast<qint64>(d));
            }
            return QString::number(d, 'g', 15);
        }
        case QJsonValue::String:
            return value.toString();
        case QJsonValue::Null:
        default:
            return QStringLiteral("null");
    }
}

}  // namespace

int ResponseBodyModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(visible_.size());
}

QVariant ResponseBodyModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(visible_.size())) {
        return {};
    }
    const Node& n =
        nodes_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(index.row())])];
    switch (role) {
        case DepthRole:
            return n.depth;
        case FieldRole:
            return n.field;
        case ValueRole:
            return n.value;
        case RawValueRole:
            return n.rawValue;
        case IsLeafRole:
            return n.isLeaf;
        case HasChildrenRole:
            return n.hasChildren;
        case CollapsedRole:
            return n.collapsed;
        case PathRole:
            return n.path;
        default:
            return {};
    }
}

QHash<int, QByteArray> ResponseBodyModel::roleNames() const {
    return {
        {DepthRole, "depth"},
        {FieldRole, "field"},
        {ValueRole, "value"},
        {RawValueRole, "rawValue"},
        {IsLeafRole, "isLeaf"},
        {HasChildrenRole, "hasChildren"},
        {CollapsedRole, "collapsed"},
        {PathRole, "path"},
    };
}

void ResponseBodyModel::walk(const QJsonValue& value,
                             const QString& field,
                             int depth,
                             const QString& path) {
    // Reserve this node's slot, then fill it after recursing so subtreeEnd can
    // be set to one-past its last descendant. Index is stable because nodes_
    // only ever grows here.
    const int idx = static_cast<int>(nodes_.size());
    nodes_.push_back(Node{});
    Node node;
    node.depth = depth;
    node.field = field;
    node.path = path;

    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        node.isLeaf = false;
        node.hasChildren = !obj.isEmpty();
        node.value = QStringLiteral("{%1}").arg(obj.size());
        // QJsonObject iterates keys in sorted order; that matches the tree's
        // addressing needs (path uses the key, not insertion order).
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            walk(it.value(), it.key(), depth + 1, path + QStringLiteral(".") + it.key());
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        node.isLeaf = false;
        node.hasChildren = !arr.isEmpty();
        node.value = QStringLiteral("[%1]").arg(arr.size());
        for (int i = 0; i < arr.size(); ++i) {
            walk(arr.at(i), QString::number(i), depth + 1, path + QStringLiteral("[%1]").arg(i));
        }
    } else {
        node.isLeaf = true;
        node.hasChildren = false;
        node.rawValue = scalarToString(value);
        node.value = node.rawValue.size() > kMaxDisplayValue
                         ? node.rawValue.left(kMaxDisplayValue) + QStringLiteral("…")
                         : node.rawValue;
    }
    node.subtreeEnd = static_cast<int>(nodes_.size());
    nodes_[static_cast<std::size_t>(idx)] = std::move(node);
}

void ResponseBodyModel::setBody(const QString& body) {
    beginResetModel();
    nodes_.clear();
    visible_.clear();
    isJson_ = false;

    if (!body.isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && !doc.isNull()) {
            isJson_ = true;
            // Don't emit a redundant root row for the top-level container —
            // expose its members at depth 0 (mirrors the previous behavior).
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                    walk(it.value(), it.key(), 0, QStringLiteral("$.") + it.key());
                }
            } else if (doc.isArray()) {
                const QJsonArray arr = doc.array();
                for (int i = 0; i < arr.size(); ++i) {
                    walk(arr.at(i), QString::number(i), 0, QStringLiteral("$[%1]").arg(i));
                }
            }
        } else {
            // Not JSON: a single raw row, so the tree view still shows the body.
            Node node;
            node.field = QStringLiteral("(not JSON)");
            node.rawValue = body;
            node.value = body.size() > kMaxDisplayValue
                             ? body.left(kMaxDisplayValue) + QStringLiteral("…")
                             : body;
            node.path = QStringLiteral("$");
            node.subtreeEnd = 1;
            nodes_.push_back(std::move(node));
        }
    }

    rebuildVisible();
    endResetModel();
    emit bodyChanged();
}

void ResponseBodyModel::reset() {
    setBody(QString{});
}

void ResponseBodyModel::rebuildVisible() {
    visible_.clear();
    visible_.reserve(nodes_.size());
    const int n = static_cast<int>(nodes_.size());
    for (int i = 0; i < n;) {
        visible_.push_back(i);
        if (nodes_[static_cast<std::size_t>(i)].collapsed &&
            nodes_[static_cast<std::size_t>(i)].hasChildren) {
            // Skip the whole collapsed subtree in O(1) via subtreeEnd.
            i = nodes_[static_cast<std::size_t>(i)].subtreeEnd;
        } else {
            ++i;
        }
    }
}

void ResponseBodyModel::toggle(int visibleRow) {
    if (visibleRow < 0 || visibleRow >= static_cast<int>(visible_.size())) {
        return;
    }
    Node& n = nodes_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(visibleRow)])];
    if (!n.hasChildren) {
        return;
    }
    beginResetModel();
    n.collapsed = !n.collapsed;
    rebuildVisible();
    endResetModel();
}

void ResponseBodyModel::expandAll() {
    beginResetModel();
    for (Node& n : nodes_) {
        n.collapsed = false;
    }
    rebuildVisible();
    endResetModel();
}

void ResponseBodyModel::collapseAll() {
    beginResetModel();
    for (Node& n : nodes_) {
        if (n.hasChildren && n.depth == 0) {
            n.collapsed = true;
        }
    }
    rebuildVisible();
    endResetModel();
}

}  // namespace reqloom::desktop::qml
