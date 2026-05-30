// CommandPalette — Cmd/Ctrl+P fuzzy launcher (DESIGN.md §6.7, PRD FR-14).
// A top-level popup (escapes panel clipping, stacks above everything but the
// tooltip) listing the project's operations; a `>` prefix switches to global
// commands. Keyboard only: type to filter, arrows to move, Enter to run, Esc
// to close. Recent operations float to the top (FR-14.3).
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

#include <vector>

class QLineEdit;
class QListWidget;
class QKeyEvent;

namespace chainapi::desktop::widgets {

/// One entry the palette can surface: a project operation or a global command.
struct PaletteItem {
    enum class Kind : std::uint8_t { Operation, GlobalCommand };

    Kind kind{Kind::Operation};
    QString id;      ///< operation id, or a global command id
    QString label;   ///< primary text shown
    QString detail;  ///< secondary text (method, hint)
};

class CommandPalette : public QWidget {
    Q_OBJECT

public:
    explicit CommandPalette(QWidget* parent = nullptr);
    ~CommandPalette() override;

    CommandPalette(const CommandPalette&) = delete;
    CommandPalette& operator=(const CommandPalette&) = delete;
    CommandPalette(CommandPalette&&) = delete;
    CommandPalette& operator=(CommandPalette&&) = delete;

    /// Replace the source pool the palette filters over.
    void setItems(std::vector<PaletteItem> operations, std::vector<PaletteItem> globalCommands);

    void setTheme(const theming::Theme& theme);

    /// Show centered over `anchor`'s top, focus the search field, clear state.
    void popUp(QWidget* anchor);

signals:
    void itemChosen(PaletteItem item);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refilter(const QString& query);
    void moveSelection(int delta);
    void acceptCurrent();
    void rememberRecent(const QString& operationId);

    QLineEdit* search_{nullptr};
    QListWidget* list_{nullptr};

    std::vector<PaletteItem> operations_;
    std::vector<PaletteItem> globalCommands_;
    std::vector<QString> recentOps_;  ///< most-recent first
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop::widgets
