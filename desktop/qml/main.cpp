// QML desktop entry point (ADR-007). Boots a QApplication + QML engine and
// loads the Reqloom module's Main window. Logic lives in C++ (AppController);
// presentation lives in QML.
//
// QApplication (not QGuiApplication) so a Widgets-only control we can't
// reasonably rebuild in QML (e.g. a QScintilla code editor) can be shown as a
// standalone Widgets *dialog window* — QML-first, Widgets only as a fallback.
#include <QtGui/QFont>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuickControls2/QQuickStyle>
#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Reqloom"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("Reqloom"));

    // Typography (theme.json): prefer Geist for UI, Noto Sans JP for Japanese
    // labels, with graceful fallback to the platform sans when not installed.
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Geist"),
                         QStringLiteral("Inter"),
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
