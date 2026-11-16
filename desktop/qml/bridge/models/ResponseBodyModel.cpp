// ResponseBodyModel — see header. A SAX pass flattens the body pre-order into `nodes_`;
// `visible_` holds the rows whose ancestors are expanded.
#include "ResponseBodyModel.h"

#include <nlohmann/json.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QHash>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace reqloom::desktop::qml {

namespace {

// Cap a leaf's display string so a huge scalar (e.g. a base64 blob) can't create a giant Text item;
// the full value stays in RawValueRole.
constexpr int kMaxDisplayValue = 512;

// The engine refuses to extract from a body nested deeper than this (kMaxJsonNestingDepth).
constexpr std::size_t kEngineMaxNesting = 256;

[[nodiscard]] QString capped(const QString& text) {
    return text.size() > kMaxDisplayValue ? text.left(kMaxDisplayValue) + QStringLiteral("…")
                                          : text;
}

// The engine's JSONPath parser ends a field name at '.' or '[' and has no quoted-key form.
[[nodiscard]] QString keyIssue(const QString& key) {
    if (key.isEmpty()) {
        return QObject::tr("An empty key cannot be written as a JSONPath.");
    }
    if (key.contains(QLatin1Char('.')) || key.contains(QLatin1Char('['))) {
        return QObject::tr("A key containing '.' or '[' cannot be written as a JSONPath.");
    }
    return {};
}

// Extraction paths with these prefixes are read from the response headers, cookies or status.
[[nodiscard]] QString sourcePrefixIssue(const QString& path) {
    if (path.startsWith(QStringLiteral("$.headers.")) ||
        path.startsWith(QStringLiteral("$.cookies.")) || path == QStringLiteral("$.status_code")) {
        return QObject::tr(
            "This path would read a response header, cookie or status, not the body.");
    }
    return {};
}

}  // namespace

class ResponseBodyBuilder final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit ResponseBodyBuilder(std::vector<ResponseBodyModel::Node>& nodes) : nodes_(nodes) {}

    bool null() override { return leaf(QStringLiteral("null")); }
    bool boolean(bool value) override {
        return leaf(value ? QStringLiteral("true") : QStringLiteral("false"));
    }
    bool number_integer(number_integer_t value) override {
        return leaf(QString::number(static_cast<qint64>(value)));
    }
    bool number_unsigned(number_unsigned_t value) override {
        return leaf(QString::number(static_cast<quint64>(value)));
    }
    // `raw` is the token as written, so precision beyond a double survives.
    bool number_float(number_float_t /*value*/, const string_t& raw) override {
        return leaf(QString::fromStdString(raw));
    }
    bool string(string_t& value) override { return leaf(QString::fromStdString(value)); }
    bool binary(binary_t& /*value*/) override { return false; }
    bool start_object(std::size_t /*elements*/) override { return open(false); }
    bool key(string_t& value) override {
        frames_.back().pendingKey = QString::fromStdString(value);
        return true;
    }
    bool end_object() override { return close(QStringLiteral("{%1}")); }
    bool start_array(std::size_t /*elements*/) override { return open(true); }
    bool end_array() override { return close(QStringLiteral("[%1]")); }
    bool parse_error(std::size_t /*position*/,
                     const std::string& /*lastToken*/,
                     const nlohmann::detail::exception& /*error*/) override {
        return false;
    }

    [[nodiscard]] std::size_t maxNesting() const { return maxNesting_; }

private:
    struct Frame {
        int node{-1};
        bool isArray{false};
        int children{0};
        int depth{0};
        QString path;
        QString issue;
        QString pendingKey;
        QHash<QString, int> keyNode;
    };

    // Append the node for the value about to be read and return its index.
    int addNode() {
        const int idx = static_cast<int>(nodes_.size());
        ResponseBodyModel::Node node;
        if (frames_.empty()) {
            node.field = QStringLiteral("(value)");
            node.path = QStringLiteral("$");
        } else {
            Frame& parent = frames_.back();
            node.depth = parent.depth;
            node.pathIssue = parent.issue;
            if (parent.isArray) {
                node.field = QString::number(parent.children);
                node.path = parent.path + QStringLiteral("[%1]").arg(parent.children);
            } else {
                node.field = parent.pendingKey;
                node.path = parent.path + QLatin1Char('.') + parent.pendingKey;
                if (node.pathIssue.isEmpty()) {
                    node.pathIssue = keyIssue(node.field);
                }
                // The engine keeps the last of duplicate keys, so earlier ones are unreachable.
                if (const auto prior = parent.keyNode.constFind(node.field);
                    prior != parent.keyNode.constEnd()) {
                    markUnreachable(prior.value());
                }
                parent.keyNode.insert(node.field, idx);
            }
            ++parent.children;
        }
        if (node.pathIssue.isEmpty()) {
            node.pathIssue = sourcePrefixIssue(node.path);
        }
        node.subtreeEnd = idx + 1;
        nodes_.push_back(std::move(node));
        return idx;
    }

    void markUnreachable(int idx) {
        const int end = nodes_[static_cast<std::size_t>(idx)].subtreeEnd;
        for (int i = idx; i < end; ++i) {
            nodes_[static_cast<std::size_t>(i)].pathIssue =
                QObject::tr("A later duplicate of this key is the one a JSONPath reaches.");
        }
    }

    bool leaf(const QString& text) {
        auto& node = nodes_[static_cast<std::size_t>(addNode())];
        node.rawValue = text;
        node.value = capped(text);
        return true;
    }

    // The top-level container gets no row: its members are shown at depth 0.
    bool open(bool isArray) {
        Frame frame;
        frame.isArray = isArray;
        if (frames_.empty()) {
            frame.path = QStringLiteral("$");
        } else {
            frame.node = addNode();
            const auto& node = nodes_[static_cast<std::size_t>(frame.node)];
            frame.depth = node.depth + 1;
            frame.path = node.path;
            frame.issue = node.pathIssue;
        }
        frames_.push_back(std::move(frame));
        maxNesting_ = std::max(maxNesting_, frames_.size());
        return true;
    }

    bool close(const QString& summaryPattern) {
        const Frame frame = std::move(frames_.back());
        frames_.pop_back();
        if (frame.node >= 0) {
            auto& node = nodes_[static_cast<std::size_t>(frame.node)];
            node.isLeaf = false;
            node.hasChildren = frame.children > 0;
            node.value = summaryPattern.arg(frame.children);
            node.subtreeEnd = static_cast<int>(nodes_.size());
        }
        return true;
    }

    std::vector<ResponseBodyModel::Node>& nodes_;
    std::vector<Frame> frames_;
    std::size_t maxNesting_{0};
};

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
            return n.pathIssue.isEmpty() ? n.path : QString{};
        case AddressableRole:
            return n.pathIssue.isEmpty();
        case PathIssueRole:
            return n.pathIssue;
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
        {AddressableRole, "addressable"},
        {PathIssueRole, "pathIssue"},
    };
}

void ResponseBodyModel::setBody(const QString& body) {
    beginResetModel();
    nodes_.clear();
    visible_.clear();
    isJson_ = false;

    if (!body.isEmpty()) {
        const QByteArray utf8 = body.toUtf8();
        ResponseBodyBuilder builder{nodes_};
        // sax_parse reports syntax errors through parse_error rather than throwing.
        isJson_ = nlohmann::json::sax_parse(utf8.constBegin(), utf8.constEnd(), &builder);
        if (isJson_ && builder.maxNesting() > kEngineMaxNesting) {
            for (Node& node : nodes_) {
                node.pathIssue = tr("The response nests deeper than %1 levels, so the engine "
                                    "will not extract from it.")
                                     .arg(static_cast<int>(kEngineMaxNesting));
            }
        }
        if (!isJson_) {
            // Not JSON: one raw row so the tree still shows the body.
            nodes_.clear();
            Node node;
            node.field = QStringLiteral("(not JSON)");
            node.rawValue = body;
            node.value = capped(body);
            node.path = QStringLiteral("$");
            node.pathIssue = tr("The body is not JSON.");
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
            i = nodes_[static_cast<std::size_t>(i)].subtreeEnd;
        } else {
            ++i;
        }
    }
}

std::vector<int> ResponseBodyModel::visibleDescendants(int node) const {
    std::vector<int> out;
    const int end = nodes_[static_cast<std::size_t>(node)].subtreeEnd;
    for (int i = node + 1; i < end;) {
        out.push_back(i);
        const Node& n = nodes_[static_cast<std::size_t>(i)];
        i = n.collapsed && n.hasChildren ? n.subtreeEnd : i + 1;
    }
    return out;
}

void ResponseBodyModel::toggle(int visibleRow) {
    if (visibleRow < 0 || visibleRow >= static_cast<int>(visible_.size())) {
        return;
    }
    const int nodeIdx = visible_[static_cast<std::size_t>(visibleRow)];
    Node& n = nodes_[static_cast<std::size_t>(nodeIdx)];
    if (!n.hasChildren) {
        return;
    }
    const auto first = visible_.begin() + visibleRow + 1;
    if (n.collapsed) {
        const std::vector<int> shown = visibleDescendants(nodeIdx);
        const int count = static_cast<int>(shown.size());
        beginInsertRows({}, visibleRow + 1, visibleRow + count);
        n.collapsed = false;
        visible_.insert(first, shown.begin(), shown.end());
        endInsertRows();
    } else {
        const auto last = std::find_if(
            first, visible_.end(), [end = n.subtreeEnd](int idx) { return idx >= end; });
        const int count = static_cast<int>(last - first);
        beginRemoveRows({}, visibleRow + 1, visibleRow + count);
        n.collapsed = true;
        visible_.erase(first, last);
        endRemoveRows();
    }
    const QModelIndex idx = index(visibleRow);
    emit dataChanged(idx, idx, {CollapsedRole});
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
