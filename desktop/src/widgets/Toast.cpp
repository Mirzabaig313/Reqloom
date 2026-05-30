// Toast — see header. Self-owning fade-in/hold/fade-out confirmation.
#include "Toast.h"

#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtWidgets/QGraphicsOpacityEffect>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>

#include <QtCore/QPropertyAnimation>

namespace chainapi::desktop::widgets {

namespace {

constexpr int kFadeInMs = 100;
constexpr int kFadeOutMs = 200;

}  // namespace

void Toast::show(QWidget* parent, const theming::Theme& theme, const QString& message, int holdMs) {
    if (parent == nullptr) {
        return;
    }
    // Self-owning: deletes itself on fade-out (see fade connection below).
    auto* toast = new Toast(parent, theme, message, holdMs);
    toast->QWidget::show();
    toast->raise();
}

Toast::Toast(QWidget* parent, const theming::Theme& theme, const QString& message, int holdMs)
    : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QHBoxLayout(this);
    const int hPad = theming::Theme::space(theming::Space::Md);
    const int vPad = theming::Theme::space(theming::Space::Sm);
    layout->setContentsMargins(hPad, vPad, hPad, vPad);

    label_ = new QLabel(message, this);
    label_->setFont(theme.font(theming::TextStyle::Body));
    layout->addWidget(label_);

    // Opaque overlay surface from tokens (no per-widget literals): overlay
    // background, inverse-ish text via primary on overlay, soft border.
    const auto& p = theme.palette();
    setStyleSheet(QStringLiteral("background-color: %1; color: %2; border: 1px solid %3; "
                                 "border-radius: 8px;")
                      .arg(p.surfaceOverlay.name(QColor::HexRgb),
                           p.textPrimary.name(QColor::HexRgb),
                           p.borderSubtle.name(QColor::HexRgb)));

    adjustSize();
    reposition();

    // Fade in (4px rise is handled by reposition's anchor; keep motion calm
    // per §9 — opacity only, exponential ease-out).
    auto* effect = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(effect);
    fade_ = new QPropertyAnimation(effect, "opacity", this);
    fade_->setDuration(kFadeInMs);
    fade_->setStartValue(0.0);
    fade_->setEndValue(1.0);
    fade_->setEasingCurve(QEasingCurve::OutQuart);
    fade_->start();

    QTimer::singleShot(holdMs + kFadeInMs, this, [this, effect]() {
        fade_->stop();
        fade_->setDuration(kFadeOutMs);
        fade_->setStartValue(effect->opacity());
        fade_->setEndValue(0.0);
        fade_->setEasingCurve(QEasingCurve::OutQuart);
        connect(fade_, &QPropertyAnimation::finished, this, &QWidget::close);
        fade_->start();
    });
}

Toast::~Toast() = default;

void Toast::reposition() {
    auto* parentWidget = qobject_cast<QWidget*>(parent());
    if (parentWidget == nullptr) {
        return;
    }
    const QSize hint = sizeHint();
    const int x = (parentWidget->width() - hint.width()) / 2;
    // Sit above the status bar, near the bottom — out of the content's way.
    const int y =
        parentWidget->height() - hint.height() - theming::Theme::space(theming::Space::Xxl);
    move(x, y);
}

}  // namespace chainapi::desktop::widgets
