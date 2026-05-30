// ResponseViewerPanel — see header. Status + headers + JSON tree / raw body.
#include "ResponseViewerPanel.h"

#include "../widgets/LineDiff.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>
#include <QtGui/QBrush>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

namespace chainapi::desktop {

namespace {

// JSONPath of a tree node, stashed so a click can copy it (FR-7.4).
constexpr int kJsonPathRole = Qt::UserRole + 1;

/// Pretty-print a JSON body with 2-space indent so a line diff is meaningful.
/// Non-JSON bodies are returned unchanged (diffed as-is).
[[nodiscard]] QString prettyJson(const QString& body) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        return body;
    }
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

/// HTML-escape a line for safe insertion into the rich-text diff view.
[[nodiscard]] QString escapeHtml(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return out;
}

/// Compact one-line rendering of a JSON scalar for the tree's value column.
[[nodiscard]] QString scalarText(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double: {
            const double d = value.toDouble();
            // Render integers without a trailing ".0" so ids read cleanly.
            // Guard the narrowing cast: only treat finite, in-range values as
            // integers — casting NaN/Inf or an out-of-range double to qint64 is
            // UB (and aborts under the UBSan debug preset).
            if (std::isfinite(d) && d >= -9.0e15 && d <= 9.0e15 &&
                d == static_cast<double>(static_cast<qint64>(d))) {
                return QString::number(static_cast<qint64>(d));
            }
            return QString::number(d);
        }
        case QJsonValue::String:
            return value.toString();
        case QJsonValue::Null:
            return QStringLiteral("null");
        default:
            return QString{};
    }
}

}  // namespace

ResponseViewerPanel::ResponseViewerPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    const int gap = theming::Theme::space(theming::Space::Sm);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(gap);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setFont(theme_.font(theming::TextStyle::Subtitle));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statusLabel_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("responseTabs"));

    bodyTree_ = new QTreeWidget(this);
    bodyTree_->setObjectName(QStringLiteral("bodyTree"));
    bodyTree_->setColumnCount(2);
    bodyTree_->setHeaderLabels({QStringLiteral("Field"), QStringLiteral("Value")});
    bodyTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    bodyTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    bodyTree_->setFont(theme_.font(theming::TextStyle::Mono));
    bodyTree_->setToolTip(QStringLiteral("Click a row to copy its JSONPath"));
    connect(bodyTree_, &QTreeWidget::itemClicked, this, &ResponseViewerPanel::onTreeItemClicked);
    tabs_->addTab(bodyTree_, QStringLiteral("Body (Tree)"));

    bodyRaw_ = new QPlainTextEdit(this);
    bodyRaw_->setObjectName(QStringLiteral("bodyRaw"));
    bodyRaw_->setReadOnly(true);
    bodyRaw_->setLineWrapMode(QPlainTextEdit::NoWrap);
    bodyRaw_->setFont(theme_.font(theming::TextStyle::Mono));
    tabs_->addTab(bodyRaw_, QStringLiteral("Body (Raw)"));

    headersView_ = new QPlainTextEdit(this);
    headersView_->setObjectName(QStringLiteral("headersView"));
    headersView_->setReadOnly(true);
    headersView_->setFont(theme_.font(theming::TextStyle::Mono));
    tabs_->addTab(headersView_, QStringLiteral("Headers"));

    diffView_ = new QTextEdit(this);
    diffView_->setObjectName(QStringLiteral("diffView"));
    diffView_->setReadOnly(true);
    diffView_->setLineWrapMode(QTextEdit::NoWrap);
    diffView_->setFont(theme_.font(theming::TextStyle::Mono));
    tabs_->addTab(diffView_, QStringLiteral("Diff"));

    layout->addWidget(tabs_, 1);

    reset();
}

ResponseViewerPanel::~ResponseViewerPanel() = default;

void ResponseViewerPanel::applyTheme(const theming::Theme& theme) {
    theme_ = theme;
    statusLabel_->setFont(theme_.font(theming::TextStyle::Subtitle));
    const QFont mono = theme_.font(theming::TextStyle::Mono);
    bodyTree_->setFont(mono);
    bodyRaw_->setFont(mono);
    headersView_->setFont(mono);
    diffView_->setFont(mono);
    // Re-render the diff so its add/remove tints follow the new theme.
    renderDiff(previousBody_, currentBody_);
    // Re-resolve the status-label colour from the last shown status so it
    // doesn't keep a stale hue after a runtime Light/Dark switch.
    if (lastStatus_ >= 0) {
        statusLabel_->setStyleSheet(
            QStringLiteral("color: %1;").arg(statusColor(lastStatus_).name(QColor::HexRgb)));
    }
    // Re-tint any secondary-text placeholder rows currently in the tree.
    for (int i = 0; i < bodyTree_->topLevelItemCount(); ++i) {
        auto* item = bodyTree_->topLevelItem(i);
        if (item->foreground(0).style() != Qt::NoBrush) {
            item->setForeground(0, QBrush(theme_.palette().textSecondary));
        }
    }
}

void ResponseViewerPanel::onTreeItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, kJsonPathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(path);
    emit jsonPathCopied(path);
}

QColor ResponseViewerPanel::statusColor(int httpStatus) const {
    // 2xx success, 3xx/4xx warning, 5xx error — DESIGN.md §2.5 status palette.
    if (httpStatus >= 500) {
        return theme_.status(theming::StatusToken::Error);
    }
    if (httpStatus >= 300) {
        return theme_.status(theming::StatusToken::Warning);
    }
    return theme_.status(theming::StatusToken::Success);
}

void ResponseViewerPanel::reset() {
    lastStatus_ = -1;
    statusLabel_->setText(QStringLiteral("No response yet"));
    statusLabel_->setStyleSheet(QString{});
    headersView_->clear();
    showBodyPlaceholder(QStringLiteral("No response yet."));
    // Note: previousBody_/currentBody_ are intentionally preserved here. reset()
    // runs before each run to clear the visible panels; the Diff tab needs the
    // prior body to survive that so a re-run can compare against it. History is
    // cleared via clearHistory() when a different project loads.
    renderDiff(previousBody_, currentBody_);
}

void ResponseViewerPanel::clearHistory() {
    previousBody_.clear();
    currentBody_.clear();
    renderDiff(previousBody_, currentBody_);
}

void ResponseViewerPanel::showBodyPlaceholder(const QString& message) {
    bodyTree_->clear();
    auto* item = new QTreeWidgetItem(bodyTree_);
    item->setText(0, message);
    item->setForeground(0, QBrush(theme_.palette().textSecondary));
    item->setFirstColumnSpanned(true);
    bodyRaw_->setPlainText(message);
}

void ResponseViewerPanel::onResponseReceived(
    int /*index*/, int status, QString headers, int bodySize, qint64 elapsedMs, QString body) {
    lastStatus_ = status;
    statusLabel_->setText(
        QStringLiteral("HTTP %1  ·  %2 bytes  ·  %3 ms").arg(status).arg(bodySize).arg(elapsedMs));
    statusLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(statusColor(status).name(QColor::HexRgb)));
    headersView_->setPlainText(headers);

    if (body.isEmpty()) {
        // Either the body was genuinely empty, or capture was off. Distinguish
        // using the reported size so the user knows whether to enable capture.
        if (bodySize > 0) {
            showBodyPlaceholder(
                QStringLiteral(
                    "Body not captured.\n\nEnable \"Capture response bodies\" in the toolbar to "
                    "view full response payloads (including this %1-byte body).")
                    .arg(bodySize));
        } else {
            showBodyPlaceholder(QStringLiteral("Empty response body."));
        }
        return;
    }
    renderBody(body);

    // Roll the body history forward and refresh the Diff tab: previous vs the
    // body just received. Pretty-print so the line diff is structural, not a
    // single-line whole-body change.
    previousBody_ = currentBody_;
    currentBody_ = prettyJson(body);
    renderDiff(previousBody_, currentBody_);
}

void ResponseViewerPanel::renderDiff(const QString& previousBody, const QString& currentBody) {
    if (previousBody.isEmpty()) {
        diffView_->setHtml(
            QStringLiteral("<span style='color:%1'>Run this operation again to compare the "
                           "response against the previous run.</span>")
                .arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
        return;
    }

    const auto lines = widgets::diff::lineDiff(previousBody, currentBody);
    const QString addBg = theme_.palette().tintDiffAdd.name(QColor::HexRgb);
    const QString removeBg = theme_.palette().tintDiffRemove.name(QColor::HexRgb);
    const QString textColor = theme_.palette().textPrimary.name(QColor::HexRgb);

    QString html = QStringLiteral("<div style='white-space:pre; font-family:monospace; color:%1'>")
                       .arg(textColor);
    for (const auto& line : lines) {
        QString bg;
        QString marker;
        switch (line.kind) {
            case widgets::diff::DiffLine::Kind::Added:
                bg = addBg;
                marker = QStringLiteral("+ ");
                break;
            case widgets::diff::DiffLine::Kind::Removed:
                bg = removeBg;
                marker = QStringLiteral("- ");
                break;
            case widgets::diff::DiffLine::Kind::Context:
                marker = QStringLiteral("  ");
                break;
        }
        const QString content = escapeHtml(marker + line.text);
        if (bg.isEmpty()) {
            html += QStringLiteral("<div>%1</div>").arg(content);
        } else {
            html += QStringLiteral("<div style='background-color:%1'>%2</div>").arg(bg, content);
        }
    }
    html += QStringLiteral("</div>");
    diffView_->setHtml(html);
}

void ResponseViewerPanel::renderBody(const QString& body) {
    bodyRaw_->setPlainText(body);

    QJsonParseError parseError{};
    const QByteArray bytes = body.toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);

    bodyTree_->clear();
    if (parseError.error != QJsonParseError::NoError) {
        // Non-JSON body (HTML error page, plain text, XML). The raw tab still
        // shows it verbatim; the tree explains why it can't structure it.
        auto* item = new QTreeWidgetItem(bodyTree_);
        item->setText(0, QStringLiteral("(not JSON — see Raw tab)"));
        item->setText(1, parseError.errorString());
        item->setForeground(0, QBrush(theme_.palette().textSecondary));
        return;
    }

    auto* root = new QTreeWidgetItem(bodyTree_);
    root->setData(0, kJsonPathRole, QStringLiteral("$"));
    if (doc.isObject()) {
        root->setText(0, QStringLiteral("{ } object"));
        populateTree(root, QStringLiteral("$"), doc.object());
    } else if (doc.isArray()) {
        root->setText(0, QStringLiteral("[ ] array"));
        populateTree(root, QStringLiteral("$"), doc.array());
    } else {
        root->setText(0, QStringLiteral("value"));
        root->setText(1, scalarText(QJsonValue::fromVariant(doc.toVariant())));
    }
    bodyTree_->expandToDepth(2);
}

void ResponseViewerPanel::populateTree(QTreeWidgetItem* parent,
                                       const QString& path,
                                       const QJsonValue& value) {
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, it.key());
            // Bracket keys that aren't bare identifiers so the path stays
            // valid. Escape backslashes before quotes so both survive.
            static const QRegularExpression bareKey(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
            QString childPath;
            if (bareKey.match(it.key()).hasMatch()) {
                childPath = QStringLiteral("%1.%2").arg(path, it.key());
            } else {
                QString escaped = it.key();
                escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
                escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
                childPath = QStringLiteral("%1['%2']").arg(path, escaped);
            }
            child->setData(0, kJsonPathRole, childPath);
            if (it.value().isObject() || it.value().isArray()) {
                populateTree(child, childPath, it.value());
            } else {
                child->setText(1, scalarText(it.value()));
            }
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, QStringLiteral("[%1]").arg(i));
            const QString childPath = QStringLiteral("%1[%2]").arg(path).arg(i);
            child->setData(0, kJsonPathRole, childPath);
            if (arr[i].isObject() || arr[i].isArray()) {
                populateTree(child, childPath, arr[i]);
            } else {
                child->setText(1, scalarText(arr[i]));
            }
        }
    } else {
        parent->setText(1, scalarText(value));
    }
}

}  // namespace chainapi::desktop
