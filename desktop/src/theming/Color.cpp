// Color — see header. OKLCH → linear sRGB → gamma sRGB, per the standard
// Oklab matrices (Björn Ottosson). DESIGN.md §2.7 notes Qt receives sRGB hex.
#include "Color.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace reqloom::desktop::theming {

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

namespace {

/// sRGB 8-bit channel → linear light (WCAG relative-luminance definition).
[[nodiscard]] double srgbToLinear(int channel8) {
    const double c = channel8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

}  // namespace

double relativeLuminance(const QColor& color) {
    return (0.2126 * srgbToLinear(color.red())) + (0.7152 * srgbToLinear(color.green())) +
           (0.0722 * srgbToLinear(color.blue()));
}

double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double lighter = std::max(la, lb);
    const double darker = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

QColor oklchForContrast(double chroma,
                        double hueDegrees,
                        const QColor& background,
                        double targetRatio,
                        double startLightness) {
    startLightness = std::clamp(startLightness, 0.0, 1.0);

    // Already accessible at the designed lightness → keep the design.
    if (contrastRatio(oklch(startLightness, chroma, hueDegrees), background) >= targetRatio) {
        return oklch(startLightness, chroma, hueDegrees);
    }

    // On a light background contrast grows as the foreground darkens; on a dark
    // background as it lightens. Move toward the extreme that increases it.
    const bool darken = relativeLuminance(background) >= 0.5;
    const double extreme = darken ? 0.0 : 1.0;
    const double designEdge = darken ? 1.0 : 0.0;

    // Relax chroma only if even the extreme lightness can't reach the target at
    // the full designed chroma (keeps colours as vivid as accessibility allows).
    for (int step = 0; step <= 10; ++step) {
        const double c = chroma * (1.0 - (static_cast<double>(step) * 0.1));
        if (contrastRatio(oklch(extreme, c, hueDegrees), background) < targetRatio) {
            continue;  // unreachable at this chroma — desaturate and retry
        }
        // Binary-search the lightness boundary closest to the designed value:
        // `meet` always satisfies the target, `fail` does not.
        double meet = extreme;
        double fail = designEdge;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (meet + fail);
            if (contrastRatio(oklch(mid, c, hueDegrees), background) >= targetRatio) {
                meet = mid;
            } else {
                fail = mid;
            }
        }
        return oklch(meet, c, hueDegrees);
    }

    // Fallback: pure black/white guarantees any reasonable ratio.
    return oklch(extreme, 0.0, hueDegrees);
}

}  // namespace reqloom::desktop::theming
