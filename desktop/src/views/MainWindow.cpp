#include "MainWindow.h"

#include <QtWidgets/QLabel>

namespace chainapi::desktop {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("ChainAPI"));
    resize(1280, 800);

    auto* placeholder = new QLabel(QStringLiteral("ChainAPI\n\nUI panels land in Phase 2."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);
}

MainWindow::~MainWindow() = default;

}  // namespace chainapi::desktop
