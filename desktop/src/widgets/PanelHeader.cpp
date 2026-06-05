// PanelHeader — see header. Title strip with an optional subtitle line and
// trailing actions.
#include "PanelHeader.h"

#include <QtGui/QColor>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

namespace reqloom::desktop::widgets {

PanelHeader::PanelHeader(const QString& title, QWidget* parent) : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    const int hPad = theming::Theme::space(theming::Space::Sm);
    const int vPad = theming::Theme::space(theming::Space::Xs);
    layout_->setContentsMargins(hPad, vPad, hPad, vPad);
    layout_->setSpacing(theming::Theme::space(theming::Space::Sm));

    // Title + optional subtitle stack vertically in a text column.
    auto* textColumn = new QVBoxLayout;
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(0);

    titleLabel_ = new QLabel(title, this);
    titleLabel_->setFont(theme_.font(theming::TextStyle::Title));
    textColumn->addWidget(titleLabel_);

    subtitleLabel_ = new QLabel(this);
    subtitleLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    subtitleLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
    subtitleLabel_->setVisible(false);
    textColumn->addWidget(subtitleLabel_);

    layout_->addLayout(textColumn);
    layout_->addStretch(1);
}

PanelHeader::~PanelHeader() = default;

void PanelHeader::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

void PanelHeader::setSubtitle(const QString& subtitle) {
    subtitleLabel_->setText(subtitle);
    subtitleLabel_->setVisible(!subtitle.isEmpty());
}

void PanelHeader::addTrailingWidget(QWidget* widget) {
    // addWidget reparents into this header's layout; no manual setParent.
    layout_->addWidget(widget);
}

void PanelHeader::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    titleLabel_->setFont(theme_.font(theming::TextStyle::Title));
    subtitleLabel_->setFont(theme_.font(theming::TextStyle::Caption));
    subtitleLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
}

}  // namespace reqloom::desktop::widgets
