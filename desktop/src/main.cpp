// Desktop entry point. Boots the QApplication and hands off to App.
#include "application/App.h"

#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    qtApp.setApplicationName(QStringLiteral("ChainAPI"));
    qtApp.setApplicationVersion(QStringLiteral("0.1.0"));
    qtApp.setOrganizationName(QStringLiteral("ChainAPI"));

    chainapi::desktop::App app;
    app.show();
    return QApplication::exec();
}
