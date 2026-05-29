#pragma once

#include <memory>

namespace chainapi::desktop {

class MainWindow;
class Bootstrapper;
class ProjectModel;

/// Top-level application object. Owns the engine wiring (via Bootstrapper),
/// the loaded-project model, and the main window.
class App {
public:
    App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    ~App();

    void show();

private:
    std::unique_ptr<Bootstrapper> bootstrapper_;
    std::unique_ptr<ProjectModel> project_;
    std::unique_ptr<MainWindow> mainWindow_;
};

}  // namespace chainapi::desktop
