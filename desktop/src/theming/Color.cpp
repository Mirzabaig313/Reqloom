// Color — see header. OKLCH → linear sRGB → gamma sRGB, per the standard
// Oklab matrices (Björn Ottosson). DESIGN.md §2.7 notes Qt receives sRGB hex.
#include "Color.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace chainapi::desktop::theming {

namespace {

/// Linear-light channel → gamma-encoded sRGB (IEC 61966-2-1).
[[nodiscard]] double linearToSrgb(double c) {
    if (c <= 0.0031308) {
        return 12.92 * c;
    }
    return (1.055 * std::pow(c, 1.0 / 2.4)) - 0.055;
}

[[nodiscard]] int toByte(double channel01) {
    const double clamped = std::clamp(channel01, 0.0, 1.0);
    return static_cast<int>(std::lround(clamped * 255.0));
}

}  // namespace

QColor oklch(double lightness, double chroma, double hueDegrees) {
    // OKLCH → OKLab (polar to cartesian on the a/b plane).
    const double hueRadians = hueDegrees * (std::numbers::pi / 180.0);
    const double aLab = chroma * std::cos(hueRadians);
    const double bLab = chroma * std::sin(hueRadians);

    // OKLab → LMS (cubed). Matrix from Ottosson's reference implementation.
    const double lTerm = lightness + (0.396'337'777'4 * aLab) + (0.215'803'757'3 * bLab);
    const double mTerm = lightness - (0.105'561'345'8 * aLab) - (0.063'854'172'8 * bLab);
    const double sTerm = lightness - (0.089'484'177'5 * aLab) - (1.291'485'548'0 * bLab);

    const double lCubed = lTerm * lTerm * lTerm;
    const double mCubed = mTerm * mTerm * mTerm;
    const double sCubed = sTerm * sTerm * sTerm;

    // LMS → linear sRGB.
    const double rLinear =
        (4.076'741'662'1 * lCubed) - (3.307'711'591'3 * mCubed) + (0.230'969'929'2 * sCubed);
    const double gLinear =
        (-1.268'438'004'6 * lCubed) + (2.609'757'401'1 * mCubed) - (0.341'319'396'5 * sCubed);
    const double bLinear =
        (-0.004'196'086'3 * lCubed) - (0.703'418'614'7 * mCubed) + (1.707'614'701'0 * sCubed);

    QColor out;
    out.setRgb(toByte(linearToSrgb(rLinear)),
               toByte(linearToSrgb(gLinear)),
               toByte(linearToSrgb(bLinear)));
    return out;
}

QString oklchHex(double lightness, double chroma, double hueDegrees) {
    return oklch(lightness, chroma, hueDegrees).name(QColor::HexRgb);
}

}  // namespace chainapi::desktop::theming
