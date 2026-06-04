// ChainView — see header. Vertical node list with method-coloured pills and
// connector glyphs between steps.
#include "ChainView.h"

#include "../views/Formatting.h"

#include <QtCore/Qt>
#include <QtGui/QColor>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop::widgets {

namespace {

/// Map a verb to the dynamic `methodClass` value the central sheet colours.
[[nodiscard]] QString methodClass(const QString& method) {
    switch (format::methodColor(method)) {
        case theming::MethodColor::Get:
            return QStringLiteral("get");
        case theming::MethodColor::Post:
            return QStringLiteral("post");
        case theming::MethodColor::Put:
            return QStringLiteral("put");
        case theming::MethodColor::Patch:
            return QStringLiteral("patch");
        case theming::MethodColor::Delete:
            return QStringLiteral("delete");
        case theming::MethodColor::Neutral:
            return QString{};
    }
    return QString{};
}

}  // namespace

ChainView::ChainView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("chainView"));
    setAttribute(Qt::WA_StyledBackground, true);
    body_ = new QVBoxLayout(this);
    const int pad = theming::Theme::space(theming::Space::Sm);
    body_->setContentsMargins(pad, pad, pad, pad);
    body_->setSpacing(0);
    rebuild();
}

ChainView::~ChainView() = default;

void ChainView::setNodes(const std::vector<Node>& nodes) {
    nodes_ = nodes;
    emptyMessage_.clear();
    rebuild();
}

void ChainView::setEmptyMessage(const QString& message) {
    nodes_.clear();
    emptyMessage_ = message;
    rebuild();
}

void ChainView::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    rebuild();
}

void ChainView::rebuild() {
    // Tear down prior rows synchronously. rebuild() is only ever called from
    // the external setters (setNodes/setEmptyMessage/setTheme), never from a
    // child widget's own signal, so immediate delete is safe and avoids the
    // brief ghosting deleteLater() would leave (a detached-but-unhidden row
    // keeps its geometry until the event loop runs).
    while (QLayoutItem* item = body_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    if (nodes_.empty()) {
        auto* empty = new QLabel(emptyMessage_, this);
        empty->setWordWrap(true);
        empty->setFont(theme_.font(theming::TextStyle::Caption));
        empty->setStyleSheet(
            QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
        body_->addWidget(empty);
        return;
    }

    const QFont mono = theme_.font(theming::TextStyle::Mono);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];

        auto* rowWidget = new QWidget(this);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(theming::Theme::space(theming::Space::Sm));

        // Method pill — reuses the central #methodPill styling via object name
        // + methodClass property so it matches the explorer + address bar.
        auto* pill = new QLabel(node.method, rowWidget);
        pill->setObjectName(QStringLiteral("methodPill"));
        pill->setAlignment(Qt::AlignCenter);
        pill->setProperty("methodClass", methodClass(node.method));
        pill->setMinimumWidth(58);
        row->addWidget(pill);

        auto* name = new QLabel(node.operationId, rowWidget);
        name->setFont(mono);
        if (node.isTarget) {
            // The target is the operation the user invoked — give it weight.
            name->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;")
                                    .arg(theme_.palette().textPrimary.name(QColor::HexRgb)));
        } else {
            name->setStyleSheet(QStringLiteral("color: %1;")
                                    .arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
        }
        row->addWidget(name, 1);

        if (node.isTarget) {
            auto* badge = new QLabel(QStringLiteral("target"), rowWidget);
            badge->setFont(theme_.font(theming::TextStyle::Caption));
            badge->setStyleSheet(
                QStringLiteral("color: %1;").arg(theme_.palette().accentBase.name(QColor::HexRgb)));
            row->addWidget(badge);
        }

        body_->addWidget(rowWidget);

        // Connector glyph between steps (not after the last).
        if (i + 1 < nodes_.size()) {
            auto* connector = new QLabel(QStringLiteral("↓"), this);
            connector->setAlignment(Qt::AlignLeft);
            connector->setContentsMargins(theming::Theme::space(theming::Space::Md), 0, 0, 0);
            connector->setStyleSheet(QStringLiteral("color: %1;")
                                         .arg(theme_.palette().borderStrong.name(QColor::HexRgb)));
            body_->addWidget(connector);
        }
    }
    body_->addStretch(1);
}

}  // namespace chainapi::desktop::widgets
