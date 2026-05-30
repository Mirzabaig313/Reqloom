// PanelHeader — see header. Title strip with optional trailing actions.
#include "PanelHeader.h"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>

namespace chainapi::desktop::widgets {

PanelHeader::PanelHeader(const QString& title, QWidget* parent) : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    const int hPad = theming::Theme::space(theming::Space::Sm);
    const int vPad = theming::Theme::space(theming::Space::Xs);
    layout_->setContentsMargins(hPad, vPad, hPad, vPad);
    layout_->setSpacing(theming::Theme::space(theming::Space::Sm));

    titleLabel_ = new QLabel(title, this);
    titleLabel_->setFont(theme_.font(theming::TextStyle::Subtitle));
    layout_->addWidget(titleLabel_);
    layout_->addStretch(1);
}

PanelHeader::~PanelHeader() = default;

void PanelHeader::setTitle(const QString& title) {
    titleLabel_->setText(title);
}

void PanelHeader::addTrailingWidget(QWidget* widget) {
    widget->setParent(this);
    layout_->addWidget(widget);
}

void PanelHeader::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    titleLabel_->setFont(theme_.font(theming::TextStyle::Subtitle));
}

}  // namespace chainapi::desktop::widgets
