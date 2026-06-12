// WCAG contrast guard for the status + method colour vocabularies. Badges pair
// a coloured foreground (status hue / method hue) with a low-emphasis tinted
// background of the same hue; this test computes the WCAG 2.x contrast ratio
// for each pairing in both themes and asserts a ≥3:1 floor (AA for bold/large
// text and for UI-component contrast, §1.4.3/§1.4.11). Status badges also pair
// colour with a glyph (DESIGN.md §6.1), so colour is never the sole signal.
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

constexpr double kMinRatio = 3.0;

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

}  // namespace reqloom::desktop::theming::tests
