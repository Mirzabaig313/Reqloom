// StatusBadge — see header. Custom-painted dot/pill with glyph + colour.
#include "StatusBadge.h"

#include <QtCore/QVariantAnimation>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

namespace chainapi::desktop::widgets {

namespace {

constexpr int kDotDiameter = 8;
constexpr int kPillRadius = 6;

}  // namespace

QString StatusBadge::glyph(theming::StatusToken status) {
    // A distinct mark per state so colour is never the only signal (§6.1).
    switch (status) {
        case theming::StatusToken::Idle:
            return QStringLiteral("○");  // empty circle: not yet run
        case theming::StatusToken::Running:
            return QStringLiteral("●");  // filled dot: in flight
        case theming::StatusToken::Success:
            return QStringLiteral("✓");
        case theming::StatusToken::Warning:
            return QStringLiteral("▲");  // triangle: attention (null extract)
        case theming::StatusToken::Error:
            return QStringLiteral("✕");
        case theming::StatusToken::Cancelled:
            return QStringLiteral("⊘");  // slashed circle
        case theming::StatusToken::Blocked:
            return QStringLiteral("⏸");  // pause: upstream failed, never ran
        case theming::StatusToken::Skipped:
            return QStringLiteral("–");  // dash: conditionally skipped
    }
    return QStringLiteral("○");
}

StatusBadge::StatusBadge(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

StatusBadge::~StatusBadge() = default;

void StatusBadge::setVariant(Variant variant) {
    if (variant_ == variant) {
        return;
    }
    variant_ = variant;
    updateGeometry();
    update();
}

void StatusBadge::setStatus(theming::StatusToken status) {
    if (status_ == status) {
        return;
    }
    status_ = status;
    setAccessibleName(glyph(status_));  // screen readers get the state mark
    updatePulse();
    update();
}

void StatusBadge::setText(const QString& text) {
    if (text_ == text) {
        return;
    }
    text_ = text;
    updateGeometry();
    update();
}

void StatusBadge::setTheme(const theming::Theme& theme) {
    theme_ = theme;
    update();
}

void StatusBadge::updatePulse() {
    const bool shouldPulse = (status_ == theming::StatusToken::Running);

    if (!shouldPulse) {
        if (pulse_ != nullptr) {
            pulse_->stop();
        }
        pulseOpacity_ = 1.0;
        return;
    }

    if (pulse_ == nullptr) {
        // 1.2s opacity loop, the one ambient animation in the app (§9).
        pulse_ = new QVariantAnimation(this);
        pulse_->setStartValue(0.55);
        pulse_->setEndValue(1.0);
        pulse_->setDuration(600);
        pulse_->setLoopCount(-1);
        pulse_->setEasingCurve(QEasingCurve::InOutSine);
        connect(pulse_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            pulseOpacity_ = v.toDouble();
            update();
        });
        // Ping-pong by flipping direction at each loop boundary.
        connect(pulse_, &QVariantAnimation::currentLoopChanged, this, [this](int) {
            const auto dir = pulse_->direction();
            pulse_->setDirection(dir == QAbstractAnimation::Forward ? QAbstractAnimation::Backward
                                                                    : QAbstractAnimation::Forward);
        });
    }
    pulse_->start();
}

QSize StatusBadge::sizeHint() const {
    if (variant_ == Variant::Dot) {
        return {kDotDiameter + 4, kDotDiameter + 4};
    }
    const QFontMetrics metrics(font());
    const QString label =
        glyph(status_) + (text_.isEmpty() ? QString{} : QStringLiteral(" ") + text_);
    const int width =
        metrics.horizontalAdvance(label) + (theming::Theme::space(theming::Space::Sm) * 2);
    const int height = metrics.height() + theming::Theme::space(theming::Space::Xs);
    return {width, height};
}

void StatusBadge::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(pulseOpacity_);

    const QColor color = theme_.status(status_);

    if (variant_ == Variant::Dot) {
        const int d = kDotDiameter;
        const QRectF dot((width() - d) / 2.0, (height() - d) / 2.0, d, d);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(dot);
        return;
    }

    // Pill: tinted rounded rect + glyph and label in the status colour.
    QColor fill = color;
    fill.setAlpha(38);  // the sanctioned see-through is the running pulse only;
                        // a faint status wash here keeps the pill legible on
                        // any surface without a second token. Border carries
                        // the solid colour so contrast holds.
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.addRoundedRect(rect, kPillRadius, kPillRadius);
    painter.fillPath(path, fill);
    painter.setPen(color);
    painter.drawPath(path);

    const QString label =
        glyph(status_) + (text_.isEmpty() ? QString{} : QStringLiteral("  ") + text_);
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignCenter, label);
}

}  // namespace chainapi::desktop::widgets
