
// Theme — resolved design tokens for one appearance (light or dark).
// DESIGN.md is the source of truth: §2.4 semantic colors, §2.5 status palette,
// §2.9 tint tokens, §4.2 type scale, §5.1 spacing scale. Widgets and the QSS
// reference these resolved values, never raw OKLCH/hex literals (§13).
#pragma once

#include <QtGui/QColor>
#include <QtGui/QFont>

#include <cstdint>

namespace chainapi::desktop::theming {

enum class Appearance : std::uint8_t { Light, Dark };

/// Status vocabulary (DESIGN.md §2.5). Maps 1:1 to engine state enums so the
/// same hue means the same thing in every panel.
enum class StatusToken : std::uint8_t {
    Idle,
    Running,
    Success,
    Warning,
    Error,
    Cancelled,
    Blocked,
    Skipped,
};

/// Resolved semantic colors for the active appearance (DESIGN.md §2.4).
struct Palette {
    QColor surfaceBase;
    QColor surfaceRaised;
    QColor surfaceSunken;
    QColor surfaceOverlay;
    QColor borderSubtle;
    QColor borderStrong;
    QColor textPrimary;
    QColor textSecondary;
    QColor textDisabled;
    QColor textInverse;
    QColor accentBase;
    QColor accentHover;
    QColor accentMuted;

    // Status palette (§2.5).
    QColor statusIdle;
    QColor statusRunning;
    QColor statusSuccess;
    QColor statusWarning;
    QColor statusError;
    QColor statusCancelled;
    QColor statusBlocked;

    // Tint tokens (§2.9) — precomputed opaque values, never alpha composites.
    QColor tintCache;
    QColor tintSubstituted;
    QColor tintDiffAdd;
    QColor tintDiffRemove;
};

/// Spacing scale step (DESIGN.md §5.1).
enum class Space : std::uint8_t { Xs, Sm, Md, Lg, Xl, Xxl };

/// Type-scale role (DESIGN.md §4.2).
enum class TextStyle : std::uint8_t { Title, Subtitle, Body, Label, Caption, Mono };

/// The full token set for one appearance. Construct via `Theme::resolve`.
class Theme {
public:
    [[nodiscard]] static Theme resolve(Appearance appearance);

    [[nodiscard]] Appearance appearance() const noexcept { return appearance_; }
    [[nodiscard]] const Palette& palette() const noexcept { return palette_; }

    /// Status hue resolved to a QColor for the active appearance.
    [[nodiscard]] QColor status(StatusToken token) const noexcept;

    /// An opaque, low-emphasis background tint for a status (DESIGN.md §2.9:
    /// precomputed opaque values, never alpha composites). Used as the fill
    /// behind status pills and chips so they stay legible on any surface.
    [[nodiscard]] QColor statusTint(StatusToken token) const noexcept;

    /// Spacing in device-independent px (pre-OS-scaling; Qt scales fonts/DPI).
    [[nodiscard]] static int space(Space step) noexcept;

    /// A font for the given role, derived from the system base font so OS
    /// font scaling is respected (NFR-5.4). `mono` uses the platform monospace.
    [[nodiscard]] QFont font(TextStyle style) const;

    /// The complete application stylesheet for this appearance, built from the
    /// resolved palette. Applied via qApp->setStyleSheet on theme change.
    [[nodiscard]] QString styleSheet() const;

private:
    Theme() = default;

    Appearance appearance_{Appearance::Dark};
    Palette palette_;
};

}  // namespace chainapi::desktop::theming
