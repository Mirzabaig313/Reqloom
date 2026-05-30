#include "App.h"

#include "../theming/ThemeManager.h"
#include "../views/MainWindow.h"
#include "Bootstrapper.h"
#include "ProjectModel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QString>

namespace chainapi::desktop {

namespace {

/// Locate the bundled MarketplaceAPI sample so first-run is useful without
/// a file dialog (PRD §12). Walks up from the executable directory looking
/// for `samples/marketplace/chainapi.yaml`; returns empty if not found
/// (e.g. an installed bundle without samples) — the app then opens empty.
[[nodiscard]] QString locateSampleProject() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int hops = 0; hops < 8; ++hops) {
        const QString candidate = dir.filePath(QStringLiteral("samples/marketplace/chainapi.yaml"));
        if (QFileInfo::exists(candidate)) {
            return dir.filePath(QStringLiteral("samples/marketplace"));
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

}  // namespace

App::App()
    : bootstrapper_(std::make_unique<Bootstrapper>()),
      themeManager_(std::make_unique<theming::ThemeManager>()),
      project_(std::make_unique<ProjectModel>()),
      mainWindow_(
          std::make_unique<MainWindow>(bootstrapper_->engine(), *project_, *themeManager_)) {
    // Resolve + apply the saved appearance before the window paints, so the
    // first frame is already themed (no flash of default Qt gray).
    themeManager_->start();
}

App::~App() = default;

void App::show() {
    mainWindow_->show();

    if (const QString sample = locateSampleProject(); !sample.isEmpty()) {
        mainWindow_->openProjectDirectory(sample);
    }
}

}  // namespace chainapi::desktop
