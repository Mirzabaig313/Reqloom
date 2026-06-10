// Shared GoogleTest entry point for the desktop logic tests. A QGuiApplication
// is created (offscreen via QT_QPA_PLATFORM in the test environment) because
// some reused types (Color/Theme, QSettings-backed settings) expect a Qt
// application instance. Replaces the main() that used to live in the now-retired
// Widgets UI test files (Phase 5 of the QML Migration Roadmap).
#include <gtest/gtest.h>

#include <QtGui/QGuiApplication>

int main(int argc, char** argv) {
    const QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
