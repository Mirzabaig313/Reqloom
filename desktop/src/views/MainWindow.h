// Shell window. Assembles the explorer / request editor / response / timeline
// panels into the three-pane layout (PRD §9.1) and wires them to the
// RunController and ProjectModel. Owns no engine state itself — the App
// constructs the engine + model and hands them in.
#pragma once

#include "../application/LayoutSettings.h"

#include <QtWidgets/QMainWindow>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QSplitter;
class QStackedWidget;

namespace chainapi::engine {
class ExecutionEngine;
}  // namespace chainapi::engine

namespace chainapi::desktop {

namespace theming {
class ThemeManager;
class Theme;
}  // namespace theming

namespace widgets {
class EmptyState;
}  // namespace widgets

class ProjectModel;
class RunController;
class SecretManager;
class ProjectExplorerWidget;
class RequestEditorPanel;
class ResponseViewerPanel;
class TimelinePanel;
struct RunReport;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(engine::ExecutionEngine& engine,
               ProjectModel& project,
               theming::ThemeManager& themeManager,
               QWidget* parent = nullptr);
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;
    ~MainWindow() override;

    /// Load a project directory and refresh the UI. Safe to call at startup
    /// with the bundled sample.
    void openProjectDirectory(const QString& directory);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildLayout();
    void buildMenusAndToolbar();
    void buildAppearanceMenu();
    void buildDensityMenu();
    void connectSignals();

    void onOpenProject();
    void onManageSecrets();
    void onThemeChanged(const theming::Theme& theme);
    void onProjectLoaded();
    void onProjectLoadFailed(const QString& code, const QString& detail);
    void onRunRequested(const QString& operationId, bool clean, bool dryRun);
    void onRunningChanged(bool running);
    void onRunFinished(const RunReport& report);

    /// Per-project active-environment persistence, keyed by project path
    /// in QSettings. Restored on load, saved when the user changes it.
    [[nodiscard]] QString loadSavedEnvironment() const;
    void saveSelectedEnvironment(const QString& env);

    void restoreSplitterSizes();
    void persistSplitterSizes();
    void applyDensity(Density density);

    ProjectModel& project_;
    RunController* runController_{nullptr};
    SecretManager* secretManager_{nullptr};
    theming::ThemeManager& themeManager_;

    QStackedWidget* rootStack_{nullptr};
    widgets::EmptyState* emptyState_{nullptr};
    QSplitter* mainSplitter_{nullptr};
    ProjectExplorerWidget* explorer_{nullptr};
    RequestEditorPanel* requestEditor_{nullptr};
    ResponseViewerPanel* responseViewer_{nullptr};
    TimelinePanel* timeline_{nullptr};

    QAction* manageSecretsAction_{nullptr};
    QComboBox* envCombo_{nullptr};
    QCheckBox* captureBodiesCheck_{nullptr};
    QLabel* statusLabel_{nullptr};
    Density density_{Density::Comfortable};

    // Set while programmatically restoring the saved environment on project
    // load, so the combo's change handler doesn't echo the value back to
    // settings (and isn't mistaken for a user selection).
    bool restoringEnv_{false};
};

}  // namespace chainapi::desktop
