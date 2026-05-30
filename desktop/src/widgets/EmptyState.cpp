// EmptyState — see header. Centered title + message + optional action.
#include "EmptyState.h"

#include <QtCore/Qt>
#include <QtGui/QColor>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop::widgets {

EmptyState::EmptyState(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->addStretch(1);

    title_ = new QLabel(this);
    title_->setAlignment(Qt::AlignCenter);
    title_->setFont(theme_.font(theming::TextStyle::Title));
    outer->addWidget(title_);

    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setFont(theme_.font(theming::TextStyle::Body));
    // Cap prose width for readability (§4.2); centering keeps it composed.
    message_->setMaximumWidth(440);
    auto* msgRow = new QHBoxLayout;
    msgRow->addStretch(1);
    msgRow->addWidget(message_);
    msgRow->addStretch(1);
    outer->addLayout(msgRow);

    outer->addSpacing(theming::Theme::space(theming::Space::Lg));

    action_ = new QPushButton(this);
    action_->setObjectName(QStringLiteral("primaryAction"));
    action_->setVisible(false);
    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch(1);
    actionRow->addWidget(action_);
    actionRow->addStretch(1);
    outer->addLayout(actionRow);

    outer->addStretch(1);

    connect(action_, &QPushButton::clicked, this, [this]() {
        if (onClick_) {
            onClick_();
        }
    });

    setTheme(theme_);
}

EmptyState::~EmptyState() = default;

void EmptyState::setTitle(const QString& title) {
    title_->setText(title);
}

void EmptyState::setMessage(const QString& message) {
    message_->setText(message);
}

void EmptyState::setAction(const QString& label, std::function<void()> onClick) {
    onClick_ = std::move(onClick);
    action_->setText(label);
    action_->setVisible(!label.isEmpty());
}

void EmptyState::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    title_->setFont(theme_.font(theming::TextStyle::Title));
    message_->setFont(theme_.font(theming::TextStyle::Body));
    // Secondary-text colour for the supporting message (token, not literal).
    message_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme_.palette().textSecondary.name(QColor::HexRgb)));
}

}  // namespace chainapi::desktop::widgets
