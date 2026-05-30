// ThemeManager — owns the active appearance mode (Light/Dark/System), applies
// the resolved Theme's stylesheet to the application, persists the choice to
// QSettings, and switches live without restart (DESIGN.md §3, §13).
//
// System mode follows the OS color scheme via Qt 6.5+ QStyleHints::colorScheme
// and reacts to changes at runtime.
#pragma once

#include "Theme.h"

#include <QtCore/QObject>

#include <cstdint>

namespace chainapi::desktop::theming {

class ThemeManager : public QObject {
    Q_OBJECT

public:
    /// User-facing appearance preference (DESIGN.md §3). System follows the OS.
    enum class Mode : std::uint8_t { Light, Dark, System };

    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override;

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;
    ThemeManager(ThemeManager&&) = delete;
    ThemeManager& operator=(ThemeManager&&) = delete;

    /// Resolve the saved preference, apply the matching stylesheet to the
    /// application, and begin following the OS when in System mode. Call once
    /// after the QApplication exists.
    void start();

    /// Switch the preference, apply live, and persist it.
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    /// The currently applied theme (resolved appearance, post System mapping).
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }

signals:
    /// Emitted after a new theme is applied, so widgets that cache palette-
    /// derived assets (status colors, custom-painted atoms) can refresh.
    void themeChanged(const Theme& theme);

private:
    void apply();
    [[nodiscard]] Appearance effectiveAppearance() const;

    Mode mode_{Mode::System};
    Theme theme_{Theme::resolve(Appearance::Dark)};
};

}  // namespace chainapi::desktop::theming
