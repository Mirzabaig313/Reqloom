// ResponseViewerPanel — see header. Status + headers + JSON tree / raw body.
#include "ResponseViewerPanel.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop {

namespace {

/// Compact one-line rendering of a JSON scalar for the tree's value column.
[[nodiscard]] QString scalarText(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double: {
            const double d = value.toDouble();
            // Render integers without a trailing ".0" so ids read cleanly.
            if (d == static_cast<double>(static_cast<qint64>(d))) {
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

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    auto statusFont = statusLabel_->font();
    statusFont.setBold(true);
    statusLabel_->setFont(statusFont);
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
    tabs_->addTab(bodyTree_, QStringLiteral("Body (Tree)"));

    bodyRaw_ = new QPlainTextEdit(this);
    bodyRaw_->setObjectName(QStringLiteral("bodyRaw"));
    bodyRaw_->setReadOnly(true);
    bodyRaw_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs_->addTab(bodyRaw_, QStringLiteral("Body (Raw)"));

    headersView_ = new QPlainTextEdit(this);
    headersView_->setObjectName(QStringLiteral("headersView"));
    headersView_->setReadOnly(true);
    tabs_->addTab(headersView_, QStringLiteral("Headers"));

    layout->addWidget(tabs_, 1);

    reset();
}

ResponseViewerPanel::~ResponseViewerPanel() = default;

void ResponseViewerPanel::reset() {
    statusLabel_->setText(QStringLiteral("No response yet"));
    statusLabel_->setStyleSheet(QString{});
    headersView_->clear();
    showBodyPlaceholder(QStringLiteral("No response yet."));
}

void ResponseViewerPanel::showBodyPlaceholder(const QString& message) {
    bodyTree_->clear();
    auto* item = new QTreeWidgetItem(bodyTree_);
    item->setText(0, message);
    item->setForeground(0, QBrush(QColor(Qt::gray)));
    item->setFirstColumnSpanned(true);
    bodyRaw_->setPlainText(message);
}

void ResponseViewerPanel::onResponseReceived(
    int /*index*/, int status, QString headers, int bodySize, qint64 elapsedMs, QString body) {
    // Colour the status by class: 2xx green, 3xx/4xx amber, 5xx red.
    QColor color{0x2E, 0x7D, 0x32};
    if (status >= 500) {
        color = QColor{0xC6, 0x28, 0x28};
    } else if (status >= 300) {
        color = QColor{0xF9, 0xA8, 0x25};
    }
    statusLabel_->setText(
        QStringLiteral("HTTP %1  ·  %2 bytes  ·  %3 ms").arg(status).arg(bodySize).arg(elapsedMs));
    statusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
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
        item->setForeground(0, QBrush(QColor(Qt::gray)));
        return;
    }

    auto* root = new QTreeWidgetItem(bodyTree_);
    if (doc.isObject()) {
        root->setText(0, QStringLiteral("{ } object"));
        populateTree(root, QString{}, doc.object());
    } else if (doc.isArray()) {
        root->setText(0, QStringLiteral("[ ] array"));
        populateTree(root, QString{}, doc.array());
    } else {
        root->setText(0, QStringLiteral("value"));
        root->setText(1, scalarText(QJsonValue::fromVariant(doc.toVariant())));
    }
    bodyTree_->expandToDepth(2);
}

void ResponseViewerPanel::populateTree(QTreeWidgetItem* parent,
                                       const QString& key,
                                       const QJsonValue& value) {
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, it.key());
            if (it.value().isObject() || it.value().isArray()) {
                populateTree(child, it.key(), it.value());
            } else {
                child->setText(1, scalarText(it.value()));
            }
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, QStringLiteral("[%1]").arg(i));
            if (arr[i].isObject() || arr[i].isArray()) {
                populateTree(child, child->text(0), arr[i]);
            } else {
                child->setText(1, scalarText(arr[i]));
            }
        }
    } else {
        parent->setText(1, scalarText(value));
    }
    Q_UNUSED(key);
}

}  // namespace chainapi::desktop
