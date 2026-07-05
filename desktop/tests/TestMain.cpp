// Shared GoogleTest entry point for the desktop logic tests. A QGuiApplication
// is created (offscreen via QT_QPA_PLATFORM in the test environment) because
// some reused types (Color/Theme, QSettings-backed settings) expect a Qt
// application instance. Replaces the main() that used to live in the now-retired
// Widgets UI test files (Phase 5 of the QML Migration Roadmap).
#include <gtest/gtest.h>

#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>

int main(int argc, char** argv) {
    // Default to the offscreen QPA plugin so the suite runs on headless CI
    // (no X server / display). Set before constructing QGuiApplication, which
    // loads the platform plugin in its constructor. This also covers ctest's
    // PRE_TEST discovery run, where the per-test ENVIRONMENT property isn't in
    // effect yet. An explicit QT_QPA_PLATFORM override is respected.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    const QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
