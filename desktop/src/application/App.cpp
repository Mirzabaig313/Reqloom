#include "App.h"

#include "../views/MainWindow.h"
#include "Bootstrapper.h"

namespace chainapi::desktop {

App::App()
    : bootstrapper_(std::make_unique<Bootstrapper>()),
      mainWindow_(std::make_unique<MainWindow>()) {}

App::~App() = default;

void App::show() {
    mainWindow_->show();
}

}  // namespace chainapi::desktop
