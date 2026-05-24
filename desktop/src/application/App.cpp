#include "App.h"

#include "Bootstrapper.h"
#include "../views/MainWindow.h"

namespace chainapi::desktop {

App::App()
    : bootstrapper_(std::make_unique<Bootstrapper>()),
      mainWindow_(std::make_unique<MainWindow>()) {}

App::~App() = default;

void App::show() {
    mainWindow_->show();
}

}  // namespace chainapi::desktop
