// StatusBadge — the most-used atom (DESIGN.md §6.1). A small dot or pill that
// encodes a status.* value as colour PLUS a glyph, never colour alone, so
// colour-blind users can still distinguish states (the app's strongest a11y
// rule, §12). The `Running` status carries the one ambient animation in the
// app: a subtle 1.2s opacity pulse that stops the instant the state changes.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QVariantAnimation;

namespace chainapi::desktop::widgets {

class StatusBadge : public QWidget {
    Q_OBJECT

public:
    /// Dot: 8px indicator for dense tree rows. Pill: glyph + label text for
    /// timelines and headers (DESIGN.md §6.1).
    enum class Variant : std::uint8_t { Dot, Pill };

    explicit StatusBadge(QWidget* parent = nullptr);
    ~StatusBadge() override;

    StatusBadge(const StatusBadge&) = delete;
    StatusBadge& operator=(const StatusBadge&) = delete;
    StatusBadge(StatusBadge&&) = delete;
    StatusBadge& operator=(StatusBadge&&) = delete;

    void setVariant(Variant variant);
    void setStatus(theming::StatusToken status);
    /// Optional trailing label for the Pill variant (ignored for Dot).
    void setText(const QString& text);
    void setTheme(const theming::Theme& theme);

    /// The glyph for a status, paired with its colour everywhere (§6.1). Shared
    /// so trees/tables that can't host a widget render the same vocabulary.
    [[nodiscard]] static QString glyph(theming::StatusToken status);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updatePulse();

    Variant variant_{Variant::Dot};
    theming::StatusToken status_{theming::StatusToken::Idle};
    QString text_;
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};

    QVariantAnimation* pulse_{nullptr};
    double pulseOpacity_{1.0};
};

}  // namespace chainapi::desktop::widgets
