
// Theme — resolved design tokens for one appearance (light or dark): semantic colours, status
// palette, tints, type scale and spacing. Widgets and QSS reference these, never raw OKLCH/hex
// literals.
#pragma once

#include <QtGui/QColor>
#include <QtGui/QFont>

#include <cstdint>

namespace reqloom::desktop::theming {

enum class Appearance : std::uint8_t { Light, Dark };

/// Status vocabulary; maps 1:1 to engine state enums so a hue means the same thing in every panel.
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

/// Resolved semantic colors for the active appearance  .
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

    // Status palette.
    QColor statusIdle;
    QColor statusRunning;
    QColor statusSuccess;
    QColor statusWarning;
    QColor statusError;
    QColor statusCancelled;
    QColor statusBlocked;

    // Tint tokens: precomputed opaque values, never alpha composites.
    QColor tintCache;
    QColor tintSubstituted;
    QColor tintDiffAdd;
    QColor tintDiffRemove;

    // HTTP method hues, distinct from status so a method chip never reads as a run state.
    QColor methodGet;
    QColor methodPost;
    QColor methodPut;
    QColor methodPatch;
    QColor methodDelete;
};

/// Spacing scale step.
enum class Space : std::uint8_t { Xs, Sm, Md, Lg, Xl, Xxl };

/// HTTP method colour: a mnemonic hue per verb (GET blue, POST green, PUT orange, PATCH yellow,
/// DELETE red, others neutral), kept separate from StatusToken so it never collides with run-state
/// meaning.
enum class MethodColor : std::uint8_t { Get, Post, Put, Patch, Delete, Neutral };

/// Type-scale role.
enum class TextStyle : std::uint8_t { Title, Subtitle, Body, Label, Caption, Mono };

/// The full token set for one appearance. Construct via `Theme::resolve`.
class Theme {
public:
    [[nodiscard]] static Theme resolve(Appearance appearance);

    [[nodiscard]] Appearance appearance() const noexcept { return appearance_; }
    [[nodiscard]] const Palette& palette() const noexcept { return palette_; }

    /// Status hue resolved to a QColor for the active appearance.
    [[nodiscard]] QColor status(StatusToken token) const noexcept;

    /// An opaque, low-emphasis background tint for a status (never an alpha composite), used behind
    /// pills and chips so they stay legible on any surface.
    [[nodiscard]] QColor statusTint(StatusToken token) const noexcept;

    /// HTTP method hue for the active appearance.
    [[nodiscard]] QColor method(MethodColor token) const noexcept;

    /// An opaque, low-emphasis tint for a method colour, mixed toward the raised surface (as
    /// statusTint); the chip fill.
    [[nodiscard]] QColor methodTint(MethodColor token) const noexcept;

    /// Spacing in device-independent px (pre-OS-scaling; Qt scales fonts/DPI).
    [[nodiscard]] static int space(Space step) noexcept;

    /// A font for the given role, derived from the system base font so OS scaling is respected.
    /// `mono` uses the platform monospace.
    [[nodiscard]] QFont font(TextStyle style) const;

    /// The complete application stylesheet for this appearance, applied via qApp->setStyleSheet on
    /// theme change.
    [[nodiscard]] QString styleSheet() const;

private:
    Theme() = default;

    Appearance appearance_{Appearance::Dark};
    Palette palette_;
};

}  // namespace reqloom::desktop::theming
