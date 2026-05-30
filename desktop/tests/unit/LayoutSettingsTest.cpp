// Tests LayoutSettings round-trips against an isolated, temp-file-backed
// QSettings (no touching the real user store).
#include "application/LayoutSettings.h"

#include <gtest/gtest.h>

#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>

namespace chainapi::desktop::tests {

namespace {

[[nodiscard]] QString tempIni(const QTemporaryDir& dir) {
    return dir.path() + QStringLiteral("/layout.ini");
}

}  // namespace

TEST(LayoutSettings, splitter_sizes_round_trip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QList<int> sizes{220, 440, 340};
    {
        QSettings settings(tempIni(dir), QSettings::IniFormat);
        LayoutSettings::saveSplitter(settings, QStringLiteral("mainSplitter"), sizes);
    }
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    EXPECT_EQ(LayoutSettings::loadSplitter(settings, QStringLiteral("mainSplitter")), sizes);
}

TEST(LayoutSettings, missing_splitter_key_returns_empty) {
    QTemporaryDir dir;
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    EXPECT_TRUE(LayoutSettings::loadSplitter(settings, QStringLiteral("nope")).isEmpty());
}

TEST(LayoutSettings, empty_inputs_are_noops) {
    QTemporaryDir dir;
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    LayoutSettings::saveSplitter(settings, QString{}, QList<int>{1, 2});
    LayoutSettings::saveSplitter(settings, QStringLiteral("k"), QList<int>{});
    EXPECT_TRUE(LayoutSettings::loadSplitter(settings, QStringLiteral("k")).isEmpty());
}

TEST(LayoutSettings, density_round_trips_and_defaults_to_comfortable) {
    QTemporaryDir dir;
    {
        QSettings fresh(tempIni(dir), QSettings::IniFormat);
        // Nothing stored yet → Comfortable default.
        EXPECT_EQ(LayoutSettings::loadDensity(fresh), Density::Comfortable);
        LayoutSettings::saveDensity(fresh, Density::Compact);
    }
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    EXPECT_EQ(LayoutSettings::loadDensity(settings), Density::Compact);
}

}  // namespace chainapi::desktop::tests
