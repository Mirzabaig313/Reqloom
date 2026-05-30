// Toast — a transient, non-blocking confirmation overlay (DESIGN.md §9, §11.4).
// Fades in, holds, fades out; positions itself near the bottom-centre of its
// parent. Used for "copied JSONPath" (FR-7.4) and, later, the undo pattern.
// Click-through and self-deleting, so callers just fire-and-forget.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QLabel;
class QPropertyAnimation;

namespace chainapi::desktop::widgets {

class Toast : public QWidget {
    Q_OBJECT

public:
    /// Show a message over `parent`, auto-dismissing after `holdMs`. The toast
    /// owns itself and deletes on fade-out, so callers don't manage lifetime.
    static void show(QWidget* parent,
                     const theming::Theme& theme,
                     const QString& message,
                     int holdMs = 1500);

    Toast(const Toast&) = delete;
    Toast& operator=(const Toast&) = delete;
    Toast(Toast&&) = delete;
    Toast& operator=(Toast&&) = delete;

private:
    Toast(QWidget* parent, const theming::Theme& theme, const QString& message, int holdMs);
    ~Toast() override;

    void reposition();

    QLabel* label_{nullptr};
    QPropertyAnimation* fade_{nullptr};
};

}  // namespace chainapi::desktop::widgets
