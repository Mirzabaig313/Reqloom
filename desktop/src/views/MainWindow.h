// Shell window. Assembles the explorer / request editor / response / timeline
// panels into the three-pane layout (PRD §9.1) and wires them to the
// RunController and ProjectModel. Owns no engine state itself — the App
// constructs the engine + model and hands them in.
#pragma once

#include "../application/LayoutSettings.h"
#include "../application/SavedResponseStore.h"

#include <QtCore/QList>
#include <QtWidgets/QMainWindow>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QToolButton;

namespace reqloom::engine {
class ExecutionEngine;
}  // namespace reqloom::engine

namespace reqloom::desktop {

namespace theming {
class ThemeManager;
class Theme;
}  // namespace theming

namespace widgets {
class EmptyState;
class CommandPalette;
struct PaletteItem;
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
    void buildShortcuts();
    void connectSignals();

    void onOpenProject();
    void onManageSecrets();
    void onThemeChanged(const theming::Theme& theme);
    void onProjectLoaded();
    void onProjectLoadFailed(const QString& code, const QString& detail);
    void onRunRequested(const QString& operationId, bool clean, bool dryRun);
    void onSaveRequested(const QString& operationId);
    void onRunningChanged(bool running);
    void onRunFinished(const RunReport& report);

    /// Persist the response panel's current response as a named example for the
    /// selected operation, then refresh the panel's examples list.
    void onSaveResponse();
    /// Push the saved examples for `operationId` into the response panel.
    void refreshSavedExamples(const QString& operationId);

    /// Context-menu actions on a saved example (from the explorer).
    void onExampleRename(const QString& operationId, const QString& name);
    void onExampleDuplicate(const QString& operationId, const QString& name);
    void onExampleDelete(const QString& operationId, const QString& name);

    void openCommandPalette();
    void onPaletteItemChosen(const widgets::PaletteItem& item);
    void runCurrentOperation(bool clean, bool dryRun);

    /// Per-project active-environment persistence, keyed by project path
    /// in QSettings. Restored on load, saved when the user changes it.
    [[nodiscard]] QString loadSavedEnvironment() const;
    void saveSelectedEnvironment(const QString& env);

    void restoreSplitterSizes();
    void persistSplitterSizes();
    void applyDensity(Density density);

    /// Collapse the explorer to a thin rail (with an expand chevron) or restore
    /// it. Persisted so the workbench reopens in the same state.
    void setExplorerCollapsed(bool collapsed);

    /// Collapse the response/timeline panel to a thin rail on the right, giving
    /// the request editor the full remaining width. Persisted.
    void setResponseCollapsed(bool collapsed);

    ProjectModel& project_;
    RunController* runController_{nullptr};
    SecretManager* secretManager_{nullptr};
    theming::ThemeManager& themeManager_;

    QStackedWidget* rootStack_{nullptr};
    widgets::EmptyState* emptyState_{nullptr};
    QSplitter* mainSplitter_{nullptr};
    QWidget* explorerRail_{nullptr};
    QWidget* responseRail_{nullptr};
    ProjectExplorerWidget* explorer_{nullptr};
    RequestEditorPanel* requestEditor_{nullptr};
    QTabWidget* rightTabs_{nullptr};
    ResponseViewerPanel* responseViewer_{nullptr};
    TimelinePanel* timeline_{nullptr};

    QAction* manageSecretsAction_{nullptr};
    QAction* toggleExplorerAction_{nullptr};
    QAction* toggleResponseAction_{nullptr};
    QMenu* viewMenu_{nullptr};
    widgets::CommandPalette* palette_{nullptr};
    QComboBox* envCombo_{nullptr};
    QCheckBox* captureBodiesCheck_{nullptr};
    QLabel* statusLabel_{nullptr};
    Density density_{Density::Comfortable};

    // Splitter sizes captured just before a pane collapses, so expanding
    // restores its previous width rather than a default.
    QList<int> preCollapseSizes_;
    QList<int> responsePreCollapseSizes_;
    bool explorerCollapsed_{false};
    bool responseCollapsed_{false};

    // Per-project saved example responses (Apidog "examples").
    SavedResponseStore savedResponses_;

    // Set while programmatically restoring the saved environment on project
    // load, so the combo's change handler doesn't echo the value back to
    // settings (and isn't mistaken for a user selection).
    bool restoringEnv_{false};
};

}  // namespace reqloom::desktop
