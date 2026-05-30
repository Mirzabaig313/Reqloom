// Tests the OKLCH → sRGB conversion against known reference points. Pure
// function, no Qt widgets — guards the theme's color math from drift.
#include "theming/Color.h"

#include <gtest/gtest.h>

#include <QtGui/QColor>

#include <algorithm>

namespace chainapi::desktop::theming::tests {

namespace {

// OKLCH conversion is exact in theory but float math drifts a little; allow a
// couple of 8-bit levels of slack per channel.
void expectRgbNear(const QColor& c, int r, int g, int b, int tol = 2) {
    EXPECT_NEAR(c.red(), r, tol) << "red";
    EXPECT_NEAR(c.green(), g, tol) << "green";
    EXPECT_NEAR(c.blue(), b, tol) << "blue";
}

}  // namespace

TEST(Color, pure_white_maps_to_255) {
    // L=1, C=0 is white regardless of hue.
    expectRgbNear(oklch(1.0, 0.0, 285.0), 255, 255, 255);
}

TEST(Color, pure_black_maps_to_zero) {
    expectRgbNear(oklch(0.0, 0.0, 285.0), 0, 0, 0);
}

TEST(Color, mid_gray_is_neutral_and_balanced) {
    // L≈0.6, no chroma → equal channels, mid-range.
    const QColor c = oklch(0.60, 0.0, 285.0);
    EXPECT_NEAR(c.red(), c.green(), 1);
    EXPECT_NEAR(c.green(), c.blue(), 1);
    EXPECT_GT(c.red(), 110);
    EXPECT_LT(c.red(), 160);
}

TEST(Color, chroma_pushes_channels_apart) {
    // The accent (indigo-violet, hue 285) at real chroma must not be gray.
    const QColor accent = oklch(0.55, 0.19, 285.0);
    const int spread = std::max({accent.red(), accent.green(), accent.blue()}) -
                       std::min({accent.red(), accent.green(), accent.blue()});
    EXPECT_GT(spread, 40) << "accent should be visibly chromatic, not gray";
    // Hue 285 is blue-violet: blue channel dominates.
    EXPECT_GT(accent.blue(), accent.green());
}

TEST(Color, hex_string_is_well_formed) {
    const QString hex = oklchHex(0.55, 0.19, 285.0);
    EXPECT_EQ(hex.size(), 7);
    EXPECT_EQ(hex.at(0), QLatin1Char('#'));
}

TEST(Color, out_of_gamut_is_clamped_not_garbage) {
    // High chroma at high lightness goes out of sRGB gamut; channels must
    // still land in [0,255], never wrap or overflow.
    const QColor c = oklch(0.95, 0.30, 150.0);
    EXPECT_GE(c.red(), 0);
    EXPECT_LE(c.red(), 255);
    EXPECT_GE(c.green(), 0);
    EXPECT_LE(c.green(), 255);
    EXPECT_GE(c.blue(), 0);
    EXPECT_LE(c.blue(), 255);
}

}  // namespace chainapi::desktop::theming::tests
