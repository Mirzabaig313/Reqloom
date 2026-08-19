// Tests layout persistence, schema validation, and legacy density migration.

#include "application/LayoutSettings.h"
#include "qml/bridge/controllers/LayoutSettingsController.h"
#include "qml/bridge/controllers/ThemeController.h"

#include <gtest/gtest.h>

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>
#include <QtCore/QVariantList>

namespace reqloom::desktop::tests {

namespace {

[[nodiscard]] QString tempIni(const QTemporaryDir& dir,
                              const QString& name = QStringLiteral("layout")) {
    return dir.path() + QLatin1Char('/') + name + QStringLiteral(".ini");
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

TEST(ThemeControllerDensityMigration, canonical_wins_legacy_only_migrates_and_invalid_falls_back) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        QSettings settings(tempIni(dir, QStringLiteral("canonical")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/density"), QStringLiteral("compact"));
        settings.setValue(QStringLiteral("layout/density"), QStringLiteral("comfortable"));
        qml::ThemeController::migrateLegacyDensity(settings);

        EXPECT_EQ(settings.value(QStringLiteral("appearance/density")).toString(),
                  QStringLiteral("compact"));
        EXPECT_FALSE(settings.contains(QStringLiteral("layout/density")));
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("legacy")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/density"), QStringLiteral(" COMPACT "));
        qml::ThemeController::migrateLegacyDensity(settings);

        EXPECT_EQ(settings.value(QStringLiteral("appearance/density")).toString(),
                  QStringLiteral("compact"));
        EXPECT_FALSE(settings.contains(QStringLiteral("layout/density")));
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("invalid-legacy")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/density"), QStringLiteral("unsupported"));
        qml::ThemeController::migrateLegacyDensity(settings);

        EXPECT_EQ(settings.value(QStringLiteral("appearance/density")).toString(),
                  QStringLiteral("comfortable"));
        EXPECT_FALSE(settings.contains(QStringLiteral("layout/density")));
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("invalid-canonical")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("appearance/density"), QStringLiteral("unsupported"));
        settings.setValue(QStringLiteral("layout/density"), QStringLiteral("compact"));
        qml::ThemeController::migrateLegacyDensity(settings);

        EXPECT_EQ(settings.value(QStringLiteral("appearance/density")).toString(),
                  QStringLiteral("comfortable"));
        EXPECT_FALSE(settings.contains(QStringLiteral("layout/density")));
    }
}

TEST(LayoutSettingsController, round_trips_named_two_positive_splitter_values) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    qml::LayoutSettingsController controller{settings};

    EXPECT_TRUE(controller.saveSplitter(QStringLiteral("main"), 220, 440));
    EXPECT_TRUE(controller.saveSplitter(QStringLiteral("centerHorizontal"), 600, 300));
    EXPECT_EQ(controller.loadSplitter(QStringLiteral("main")), QVariantList({220, 440}));
    EXPECT_EQ(controller.loadSplitter(QStringLiteral("centerHorizontal")),
              QVariantList({600, 300}));
}

TEST(LayoutSettingsController, rejects_wrong_shape_nonpositive_and_corrupt_splitter_records) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QSettings settings(tempIni(dir), QSettings::IniFormat);
    qml::LayoutSettingsController controller{settings};

    LayoutSettings::saveSplitter(settings, QStringLiteral("one"), QList<int>{100});
    LayoutSettings::saveSplitter(settings, QStringLiteral("three"), QList<int>{100, 200, 300});
    LayoutSettings::saveSplitter(settings, QStringLiteral("zero"), QList<int>{100, 0});
    LayoutSettings::saveSplitter(settings, QStringLiteral("negative"), QList<int>{100, -1});
    settings.setValue(QStringLiteral("splitterSizes/corrupt"),
                      QVariantList{QStringLiteral("bad"), 100});
    settings.setValue(QStringLiteral("splitterSizes/bool"), QVariantList{true, 100});
    settings.setValue(QStringLiteral("splitterSizes/numeric-string"),
                      QVariantList{QStringLiteral("100"), 200});
    settings.setValue(QStringLiteral("splitterSizes/floating"), QVariantList{100.0, 200});

    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("one")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("three")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("zero")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("negative")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("corrupt")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("bool")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("numeric-string")).isEmpty());
    EXPECT_TRUE(controller.loadSplitter(QStringLiteral("floating")).isEmpty());

    ASSERT_TRUE(controller.saveSplitter(QStringLiteral("valid"), 120, 240));
    EXPECT_FALSE(controller.saveSplitter(QString{}, 120, 240));
    EXPECT_FALSE(controller.saveSplitter(QStringLiteral("valid"), 0, 240));
    EXPECT_FALSE(controller.saveSplitter(QStringLiteral("valid"), 120, -1));
    EXPECT_EQ(controller.loadSplitter(QStringLiteral("valid")), QVariantList({120, 240}));
}

TEST(LayoutSettingsController, defaults_invalid_values_and_unsupported_schema) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    {
        const QString path{tempIni(dir, QStringLiteral("current"))};
        QSettings settings(path, QSettings::IniFormat);
        qml::LayoutSettingsController controller{settings};

        EXPECT_EQ(controller.schemaVersion(), 1);
        EXPECT_FALSE(controller.explorerCollapsed());
        EXPECT_FALSE(controller.responseCollapsed());
        EXPECT_EQ(controller.responseOrientation(), QStringLiteral("auto"));

        controller.setExplorerCollapsed(true);
        controller.setResponseCollapsed(true);
        controller.setResponseOrientation(QStringLiteral("vertical"));
        EXPECT_TRUE(controller.explorerCollapsed());
        EXPECT_TRUE(controller.responseCollapsed());
        EXPECT_EQ(controller.responseOrientation(), QStringLiteral("vertical"));
        EXPECT_EQ(settings.value(QStringLiteral("layout/schemaVersion")).toInt(), 1);

        controller.setResponseOrientation(QStringLiteral("unsupported"));
        EXPECT_EQ(controller.responseOrientation(), QStringLiteral("auto"));

        settings.sync();
        QSettings reopened(path, QSettings::IniFormat);
        qml::LayoutSettingsController reopenedController{reopened};
        EXPECT_TRUE(reopenedController.explorerCollapsed());
        EXPECT_TRUE(reopenedController.responseCollapsed());
        EXPECT_EQ(reopenedController.responseOrientation(), QStringLiteral("auto"));
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("future")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/schemaVersion"), 2);
        settings.setValue(QStringLiteral("layout/explorerCollapsed"), true);
        settings.setValue(QStringLiteral("layout/responseCollapsed"), true);
        settings.setValue(QStringLiteral("layout/responseOrientation"), QStringLiteral("vertical"));
        LayoutSettings::saveSplitter(settings, QStringLiteral("main"), QList<int>{100, 200});
        qml::LayoutSettingsController controller{settings};

        EXPECT_FALSE(controller.explorerCollapsed());
        EXPECT_FALSE(controller.responseCollapsed());
        EXPECT_EQ(controller.responseOrientation(), QStringLiteral("auto"));
        EXPECT_TRUE(controller.loadSplitter(QStringLiteral("main")).isEmpty());
        EXPECT_EQ(settings.value(QStringLiteral("layout/schemaVersion")).toInt(), 2);

        controller.setExplorerCollapsed(true);
        EXPECT_EQ(settings.value(QStringLiteral("layout/schemaVersion")).toInt(), 1);
        EXPECT_TRUE(controller.explorerCollapsed());
        EXPECT_FALSE(controller.responseCollapsed());
        EXPECT_EQ(controller.responseOrientation(), QStringLiteral("auto"));
        EXPECT_TRUE(controller.loadSplitter(QStringLiteral("main")).isEmpty());
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("corrupt-schema")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/schemaVersion"), QStringLiteral("bad"));
        settings.setValue(QStringLiteral("layout/explorerCollapsed"), true);
        qml::LayoutSettingsController controller{settings};

        EXPECT_FALSE(controller.explorerCollapsed());
        EXPECT_EQ(settings.value(QStringLiteral("layout/schemaVersion")).toString(),
                  QStringLiteral("bad"));
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("bool-schema")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/schemaVersion"), true);
        settings.setValue(QStringLiteral("layout/explorerCollapsed"), true);
        qml::LayoutSettingsController controller{settings};

        EXPECT_FALSE(controller.explorerCollapsed());
    }
    {
        QSettings settings(tempIni(dir, QStringLiteral("string-schema")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("layout/schemaVersion"), QStringLiteral("1"));
        settings.setValue(QStringLiteral("layout/explorerCollapsed"), true);
        qml::LayoutSettingsController controller{settings};

        EXPECT_FALSE(controller.explorerCollapsed());
    }
}

}  // namespace reqloom::desktop::tests
