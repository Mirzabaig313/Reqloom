// OKLCH → sRGB conversion. DESIGN.md §2 keeps color perceptual at the source
// (OKLCH ramps) and converts to sRGB once here, since Qt's QSS parser does not
// read OKLCH. Pure functions, no Qt-widget dependency — unit-tested.
#pragma once

#include <QtGui/QColor>

namespace reqloom::desktop::theming {

/// Convert an OKLCH color to an sRGB QColor (8-bit, opaque).
///
/// @param lightness  Perceptual lightness, 0..1.
/// @param chroma     Chroma, 0..~0.4 in practice.
/// @param hueDegrees Hue angle in degrees, 0..360.
/// Out-of-gamut results are clamped per channel into [0,1].
[[nodiscard]] QColor oklch(double lightness, double chroma, double hueDegrees);

/// Same as `oklch` but returns a `#rrggbb` string for QSS interpolation.
[[nodiscard]] QString oklchHex(double lightness, double chroma, double hueDegrees);

/// WCAG 2.x relative luminance of an sRGB colour (0..1).
[[nodiscard]] double relativeLuminance(const QColor& color);

/// WCAG 2.x contrast ratio between two colours, 1.0 (identical) .. 21.0
/// (black on white). Order-independent.
[[nodiscard]] double contrastRatio(const QColor& a, const QColor& b);

/// Solve for an OKLCH colour (keeping `chroma`/`hueDegrees`) whose sRGB form
/// meets at least `targetRatio` WCAG contrast against `background`.
///
/// Starts from `startLightness`: if that already meets the target it is
/// returned unchanged, so a palette value that is already accessible keeps its
/// designed look. Otherwise the lightness moves the minimal distance that
/// reaches the target — darker on light backgrounds, lighter on dark ones —
/// relaxing chroma only as a last resort when lightness alone cannot. This is
/// "solve for accessibility" rather than hand-tuning lightness per colour.
[[nodiscard]] QColor oklchForContrast(double chroma,
                                      double hueDegrees,
                                      const QColor& background,
                                      double targetRatio,
                                      double startLightness);

}  // namespace reqloom::desktop::theming
