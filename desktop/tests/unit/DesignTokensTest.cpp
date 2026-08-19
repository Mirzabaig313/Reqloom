// Tests density-responsive metrics and application-derived typography. Fluid Layout Plan Phases
// 1-2.
#include "qml/bridge/controllers/ThemeController.h"
#include "qml/theming/DesignTokens.h"

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>

#include <array>

namespace reqloom::desktop::qml::tests {

class DesignTokensDensityTest : public ::testing::Test {
protected:
    void SetUp() override {
        static QTemporaryDir directory{};
        ASSERT_TRUE(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        controller_ = ThemeController::create(nullptr, nullptr);
        originalDensity_ = controller_->density();
        controller_->setDensity(QStringLiteral("comfortable"));
    }

    void TearDown() override {
        if (controller_ != nullptr) {
            controller_->setDensity(originalDensity_);
        }
    }

    ThemeController* controller_{};
    QString originalDensity_{};
};

TEST_F(DesignTokensDensityTest, tracks_application_font_family_and_point_size_roles) {
    const QFont originalFont{QGuiApplication::font()};
    QFont testFont{originalFont};
    testFont.setFamily(QStringLiteral("Reqloom Phase 2 Test"));
    testFont.setPointSizeF(10.0);
    int typographyChangedCount{};
    DesignTokens tokens;
    QObject::connect(
        &tokens, &DesignTokens::typographyChanged, &tokens, [&typographyChangedCount]() {
            ++typographyChangedCount;
        });

    QGuiApplication::setFont(testFont);
    QCoreApplication::processEvents();

    EXPECT_EQ(typographyChangedCount, 1);
    EXPECT_EQ(tokens.fontSans(), QGuiApplication::font().family());
    EXPECT_DOUBLE_EQ(tokens.fontTitlePointSize(), 13.0);
    EXPECT_DOUBLE_EQ(tokens.fontSubtitlePointSize(), 11.5);
    EXPECT_DOUBLE_EQ(tokens.fontBodyPointSize(), 10.0);
    EXPECT_DOUBLE_EQ(tokens.fontLabelPointSize(), 9.2);
    EXPECT_DOUBLE_EQ(tokens.fontCaptionPointSize(), 8.5);
    EXPECT_DOUBLE_EQ(tokens.fontMonoPointSize(), 10.0);

    QGuiApplication::setFont(originalFont);
    QCoreApplication::processEvents();
}

TEST_F(DesignTokensDensityTest, converts_pixel_sized_application_font_to_positive_point_roles) {
    const QFont originalFont{QGuiApplication::font()};
    QFont testFont{originalFont};
    testFont.setPixelSize(20);
    DesignTokens tokens;

    QGuiApplication::setFont(testFont);
    QCoreApplication::processEvents();

    EXPECT_LT(QGuiApplication::font().pointSizeF(), 0.0);
    EXPECT_GT(tokens.fontBodyPointSize(), 0.0);
    EXPECT_DOUBLE_EQ(tokens.fontTitlePointSize(), tokens.fontBodyPointSize() * 1.30);

    QGuiApplication::setFont(originalFont);
    QCoreApplication::processEvents();
}

TEST_F(DesignTokensDensityTest,
       emits_once_and_exposes_compact_metrics_when_density_becomes_compact) {
    int metricsChangedCount{};
    DesignTokens tokens;
    QObject::connect(&tokens, &DesignTokens::metricsChanged, &tokens, [&metricsChangedCount]() {
        ++metricsChangedCount;
    });

    controller_->setDensity(QStringLiteral("compact"));

    EXPECT_EQ(metricsChangedCount, 1);
    EXPECT_EQ(tokens.spaceXs(), 4);
    EXPECT_EQ(tokens.spaceSm(), 6);
    EXPECT_EQ(tokens.spaceMd(), 8);
    EXPECT_EQ(tokens.spaceLg(), 12);
    EXPECT_EQ(tokens.spaceXl(), 16);
    EXPECT_EQ(tokens.spaceXxl(), 24);
    EXPECT_EQ(tokens.controlHeight(), 30);
    EXPECT_EQ(tokens.controlHeightLg(), 32);
}

TEST_F(DesignTokensDensityTest, restores_comfortable_metrics_when_density_becomes_comfortable) {
    controller_->setDensity(QStringLiteral("compact"));
    DesignTokens tokens;

    controller_->setDensity(QStringLiteral("comfortable"));

    EXPECT_EQ(tokens.spaceXs(), 4);
    EXPECT_EQ(tokens.spaceSm(), 8);
    EXPECT_EQ(tokens.spaceMd(), 12);
    EXPECT_EQ(tokens.spaceLg(), 16);
    EXPECT_EQ(tokens.spaceXl(), 24);
    EXPECT_EQ(tokens.spaceXxl(), 32);
    EXPECT_EQ(tokens.controlHeight(), 34);
    EXPECT_EQ(tokens.controlHeightLg(), 36);
}

TEST_F(DesignTokensDensityTest, preserves_non_metric_tokens_when_density_changes) {
    DesignTokens tokens;
    const std::array integerValuesBefore{
        tokens.iconSize(),
        tokens.radius(),
        tokens.radiusSm(),
        tokens.radiusLg(),
        tokens.radiusPill(),
        tokens.weightRegular(),
        tokens.weightMedium(),
        tokens.weightSemiBold(),
        tokens.weightBold(),
    };
    const std::array pointSizeValuesBefore{
        tokens.fontTitlePointSize(),
        tokens.fontSubtitlePointSize(),
        tokens.fontBodyPointSize(),
        tokens.fontLabelPointSize(),
        tokens.fontCaptionPointSize(),
        tokens.fontMonoPointSize(),
    };
    const QString fontSansBefore{tokens.fontSans()};
    const QString fontMonoBefore{tokens.fontMono()};

    controller_->setDensity(QStringLiteral("compact"));

    const std::array integerValuesAfter{
        tokens.iconSize(),
        tokens.radius(),
        tokens.radiusSm(),
        tokens.radiusLg(),
        tokens.radiusPill(),
        tokens.weightRegular(),
        tokens.weightMedium(),
        tokens.weightSemiBold(),
        tokens.weightBold(),
    };
    const std::array pointSizeValuesAfter{
        tokens.fontTitlePointSize(),
        tokens.fontSubtitlePointSize(),
        tokens.fontBodyPointSize(),
        tokens.fontLabelPointSize(),
        tokens.fontCaptionPointSize(),
        tokens.fontMonoPointSize(),
    };
    EXPECT_EQ(integerValuesAfter, integerValuesBefore);
    EXPECT_EQ(pointSizeValuesAfter, pointSizeValuesBefore);
    EXPECT_EQ(tokens.fontSans(), fontSansBefore);
    EXPECT_EQ(tokens.fontMono(), fontMonoBefore);
}

TEST_F(DesignTokensDensityTest, normalizes_unsupported_density_to_comfortable) {
    controller_->setDensity(QStringLiteral("compact"));
    ASSERT_EQ(controller_->density(), QStringLiteral("compact"));

    controller_->setDensity(QStringLiteral("unsupported"));

    EXPECT_EQ(controller_->density(), QStringLiteral("comfortable"));
}

}  // namespace reqloom::desktop::qml::tests
