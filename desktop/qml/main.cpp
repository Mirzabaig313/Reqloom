// QML desktop entry point (ADR-007). Boots a QApplication + QML engine and
// loads the Reqloom module's Main window. Logic lives in C++ (AppController);
// presentation lives in QML.
//
// QApplication (not QGuiApplication) so a Widgets-only control we can't
// reasonably rebuild in QML (e.g. a QScintilla code editor) can be shown as a
// standalone Widgets *dialog window* — QML-first, Widgets only as a fallback.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QIcon>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>
#include <QtWidgets/QApplication>

namespace {

// Register any bundled fonts (Geist for UI, Noto Sans JP for Japanese) so the
// typography from theme.json renders as designed. Scans, in order:
//   1. REQLOOM_FONT_DIR — the repo's desktop/fonts (set at build time, for dev)
//   2. <app>/fonts and the macOS bundle's Resources/fonts (for a shipped app)
// Each .ttf/.otf found is added via QFontDatabase::addApplicationFont. Missing
// fonts are a graceful no-op: the app falls back to the platform sans.
void loadBundledFonts() {
    QStringList dirs;
#ifdef REQLOOM_FONT_DIR
    dirs << QStringLiteral(REQLOOM_FONT_DIR);
#endif
    const QString exeDir = QCoreApplication::applicationDirPath();
    dirs << exeDir + QStringLiteral("/fonts");
    dirs << exeDir + QStringLiteral("/../Resources/fonts");

    for (const QString& path : dirs) {
        QDir dir(path);
        if (!dir.exists()) {
            continue;
        }
        const QStringList files = dir.entryList(
            {QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.ttc")},
            QDir::Files);
        for (const QString& file : files) {
            QFontDatabase::addApplicationFont(dir.filePath(file));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Reqloom"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("Reqloom"));

    // Window / taskbar / dock icon, bundled via the Reqloom QML module
    // resources (desktop/branding). Platforms that derive the icon from the
    // app bundle (.app/.exe) ignore this, but it covers X11/Wayland and the
    // window title bar.
    QApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/Reqloom/branding/Reqloom-app-icon.png")));

    // Load bundled fonts before setting the default font so their families
    // ("Geist", "Noto Sans JP") resolve instead of substituting.
    loadBundledFonts();

    // Typography (theme.json): prefer Geist for UI, Noto Sans JP for Japanese
    // labels, with graceful fallback to the platform sans when not installed.
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Geist"),
                         QStringLiteral("Noto Sans JP"),
                         QStringLiteral("Helvetica Neue"),
                         QStringLiteral("Arial")});
    QApplication::setFont(appFont);

    // The Basic style lets DesignTokens fully drive the palette rather than a
    // native style overriding control colors.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    engine.loadFromModule("Reqloom", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    return QApplication::exec();
}
