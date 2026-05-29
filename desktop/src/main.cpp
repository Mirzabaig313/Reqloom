// Desktop entry point. Boots the QApplication and hands off to App.
#include "application/App.h"

#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
    // The QApplication instance must outlive the UI: it owns the Qt event
    // loop driven by QApplication::exec() below.
    [[maybe_unused]] const QApplication qtApp(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ChainAPI"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("ChainAPI"));

    chainapi::desktop::App app;
    app.show();
    return QApplication::exec();
}
