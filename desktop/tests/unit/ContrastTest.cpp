// WCAG contrast guard for the status + method colour vocabularies. Badges pair
// a coloured foreground (status hue / method hue) with a low-emphasis tinted
// background of the same hue; this test computes the WCAG 2.x contrast ratio
// for each pairing in both themes and asserts a ≥4.5:1 floor (AA for normal
// text, §1.4.3). The foregrounds are solved to this ratio in Theme via
// theming::oklchForContrast, so this test locks the solver's guarantee in.
// Status badges also pair colour with a glyph, so colour is
// never the sole signal.
#include "theming/Color.h"
#include "theming/Theme.h"

#include <gtest/gtest.h>

#include <QtGui/QColor>

#include <array>
#include <cmath>

namespace reqloom::desktop::theming::tests {

namespace {

// sRGB 8-bit channel → linear light (WCAG relative-luminance definition).
[[nodiscard]] double linearize(int channel8) {
    const double c = channel8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double relativeLuminance(const QColor& c) {
    return (0.2126 * linearize(c.red())) + (0.7152 * linearize(c.green())) +
           (0.0722 * linearize(c.blue()));
}

[[nodiscard]] double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double lighter = std::max(la, lb);
    const double darker = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

// The StatusBadge (QML) fills with the status hue at 16% alpha over the raised
// surface. Mirror that composite opaquely so the test sees the real backdrop.
[[nodiscard]] QColor statusBadgeBackground(const QColor& hue, const QColor& surface) {
    constexpr double kHueWeight = 0.16;
    const auto mix = [&](int h, int s) {
        return static_cast<int>((h * kHueWeight) + (s * (1.0 - kHueWeight)));
    };
    return QColor{mix(hue.red(), surface.red()),
                  mix(hue.green(), surface.green()),
                  mix(hue.blue(), surface.blue())};
}

constexpr double kMinRatio = 4.5;

// Body/caption secondary text must clear AA (4.5:1) on every surface it can sit
// on: the base canvas, raised panels, and sunken inputs. textPrimary is the
// high-emphasis pair (trivially clears); textSecondary is the at-risk one used
// for paths, captions, subtitles, and metadata across every panel.
void checkTextContrast(Appearance appearance, const char* label) {
    const Theme theme = Theme::resolve(appearance);
    const Palette& p = theme.palette();
    const std::array<std::pair<QColor, const char*>, 3> surfaces{{
        {p.surfaceBase, "surfaceBase"},
        {p.surfaceRaised, "surfaceRaised"},
        {p.surfaceSunken, "surfaceSunken"},
    }};
    for (const auto& [surface, sname] : surfaces) {
        const double sec = contrastRatio(p.textSecondary, surface);
        EXPECT_GE(sec, kMinRatio) << label << " textSecondary on " << sname << " = " << sec << ":1";
        const double pri = contrastRatio(p.textPrimary, surface);
        EXPECT_GE(pri, kMinRatio) << label << " textPrimary on " << sname << " = " << pri << ":1";
    }
}

void checkTheme(Appearance appearance, const char* label) {
    const Theme theme = Theme::resolve(appearance);
    const Palette& p = theme.palette();

    // Method chips: method hue text on the opaque method tint.
    const std::array<std::pair<MethodColor, const char*>, 5> methods{{
        {MethodColor::Get, "GET"},
        {MethodColor::Post, "POST"},
        {MethodColor::Put, "PUT"},
        {MethodColor::Patch, "PATCH"},
        {MethodColor::Delete, "DELETE"},
    }};
    for (const auto& [token, name] : methods) {
        const double ratio = contrastRatio(theme.method(token), theme.methodTint(token));
        EXPECT_GE(ratio, kMinRatio)
            << label << " method " << name << " on its tint = " << ratio << ":1";
    }

    // Status badges: status hue text on the 16% hue-over-surface fill.
    const std::array<std::pair<StatusToken, const char*>, 5> statuses{{
        {StatusToken::Running, "running"},
        {StatusToken::Success, "success"},
        {StatusToken::Warning, "warning"},
        {StatusToken::Error, "error"},
        {StatusToken::Cancelled, "cancelled"},
    }};
    for (const auto& [token, name] : statuses) {
        const QColor hue = theme.status(token);
        const double onBadge = contrastRatio(hue, statusBadgeBackground(hue, p.surfaceRaised));
        EXPECT_GE(onBadge, kMinRatio)
            << label << " status " << name << " on its badge = " << onBadge << ":1";
    }
}

}  // namespace

TEST(Contrast, light_theme_badges_meet_aa_floor) {
    checkTheme(Appearance::Light, "light");
}

TEST(Contrast, dark_theme_badges_meet_aa_floor) {
    checkTheme(Appearance::Dark, "dark");
}

TEST(Contrast, light_theme_text_meets_aa_floor) {
    checkTextContrast(Appearance::Light, "light");
}

TEST(Contrast, dark_theme_text_meets_aa_floor) {
    checkTextContrast(Appearance::Dark, "dark");
}

// Direct solver checks: oklchForContrast must reach the requested ratio against
// both a near-white and a near-black background, across hues.
TEST(Contrast, solver_reaches_target_on_light_background) {
    const QColor lightBg = oklch(0.985, 0.003, 264.5);  // raised light surface
    for (double hue : {25.0, 95.0, 150.0, 230.0, 320.0}) {
        const QColor solved = oklchForContrast(0.18, hue, lightBg, 4.5, 0.62);
        EXPECT_GE(contrastRatio(solved, lightBg), 4.5)
            << "hue " << hue << " on light bg = " << contrastRatio(solved, lightBg);
    }
}

TEST(Contrast, solver_reaches_target_on_dark_background) {
    const QColor darkBg = oklch(0.230, 0.020, 207.3);  // raised dark surface
    for (double hue : {25.0, 95.0, 150.0, 230.0, 320.0}) {
        const QColor solved = oklchForContrast(0.18, hue, darkBg, 4.5, 0.40);
        EXPECT_GE(contrastRatio(solved, darkBg), 4.5)
            << "hue " << hue << " on dark bg = " << contrastRatio(solved, darkBg);
    }
}

TEST(Contrast, solver_keeps_already_accessible_colour_unchanged) {
    const QColor white{255, 255, 255};
    // Pure black on white already exceeds any target → returned unchanged.
    const QColor solved = oklchForContrast(0.0, 0.0, white, 4.5, 0.0);
    EXPECT_EQ(solved, oklch(0.0, 0.0, 0.0));
}

}  // namespace reqloom::desktop::theming::tests
