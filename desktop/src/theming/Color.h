// OKLCH → sRGB conversion. DESIGN.md §2 keeps color perceptual at the source
// (OKLCH ramps) and converts to sRGB once here, since Qt's QSS parser does not
// read OKLCH. Pure functions, no Qt-widget dependency — unit-tested.
#pragma once

#include <QtGui/QColor>

namespace chainapi::desktop::theming {

/// Convert an OKLCH color to an sRGB QColor (8-bit, opaque).
///
/// @param lightness  Perceptual lightness, 0..1.
/// @param chroma     Chroma, 0..~0.4 in practice.
/// @param hueDegrees Hue angle in degrees, 0..360.
/// Out-of-gamut results are clamped per channel into [0,1].
[[nodiscard]] QColor oklch(double lightness, double chroma, double hueDegrees);

/// Same as `oklch` but returns a `#rrggbb` string for QSS interpolation.
[[nodiscard]] QString oklchHex(double lightness, double chroma, double hueDegrees);

}  // namespace chainapi::desktop::theming
