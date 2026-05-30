// EmptyState — a centered "nothing here yet" panel that teaches the next step
// rather than showing a blank surface (DESIGN.md §10, PRD §12). A title, a
// secondary-text explanation, and an optional primary action button.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

#include <functional>

class QLabel;
class QPushButton;

namespace chainapi::desktop::widgets {

class EmptyState : public QWidget {
    Q_OBJECT

public:
    explicit EmptyState(QWidget* parent = nullptr);
    ~EmptyState() override;

    EmptyState(const EmptyState&) = delete;
    EmptyState& operator=(const EmptyState&) = delete;
    EmptyState(EmptyState&&) = delete;
    EmptyState& operator=(EmptyState&&) = delete;

    void setTitle(const QString& title);
    void setMessage(const QString& message);

    /// Show a primary action button with `label`, invoking `onClick` when
    /// pressed. Passing an empty label hides the button.
    void setAction(const QString& label, std::function<void()> onClick);

    void setTheme(const theming::Theme& theme);

private:
    QLabel* title_{nullptr};
    QLabel* message_{nullptr};
    QPushButton* action_{nullptr};
    std::function<void()> onClick_;
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop::widgets
