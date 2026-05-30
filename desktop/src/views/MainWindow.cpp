// MainWindow — see header. Three-pane shell + run wiring.
#include "MainWindow.h"

#include "../application/EnvironmentSettings.h"
#include "../application/LayoutSettings.h"
#include "../application/ProjectModel.h"
#include "../application/RunController.h"
#include "../application/SecretManager.h"
#include "../theming/ThemeManager.h"
#include "../widgets/EmptyState.h"
#include "ProjectExplorerWidget.h"
#include "RequestEditorPanel.h"
#include "ResponseViewerPanel.h"
#include "SecretsDialog.h"
#include "TimelinePanel.h"

#include <chainapi/engine/PublicApi.h>

#include <QtCore/QSettings>
#include <QtGui/QAction>
#include <QtGui/QActionGroup>
#include <QtGui/QCloseEvent>
#include <QtGui/QKeySequence>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QWidget>

namespace chainapi::desktop {

MainWindow::MainWindow(engine::ExecutionEngine& engine,
                       ProjectModel& project,
                       theming::ThemeManager& themeManager,
                       QWidget* parent)
    : QMainWindow(parent), project_(project), themeManager_(themeManager) {
    setWindowTitle(QStringLiteral("ChainAPI"));
    resize(1280, 800);

    runController_ = new RunController(engine, project, this);
    secretManager_ = new SecretManager(this);

    buildLayout();
    buildMenusAndToolbar();
    connectSignals();

    // Restore persisted window prefs before the first show so there's no
    // visible re-layout flash.
    QSettings settings;
    density_ = LayoutSettings::loadDensity(settings);
    applyDensity(density_);
    restoreSplitterSizes();

    // Push the already-resolved theme into the custom-painted panels so their
    // status colours and fonts match the QSS chrome from the first frame.
    onThemeChanged(themeManager_.theme());

    statusBar()->showMessage(QStringLiteral("Open a project to begin."));
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    // Persist the user's splitter layout so the workbench opens where they
    // left it. Density and environment are saved at change time.
    persistSplitterSizes();
    QMainWindow::closeEvent(event);
}

void MainWindow::buildLayout() {
    explorer_ = new ProjectExplorerWidget(this);
    requestEditor_ = new RequestEditorPanel(this);
    responseViewer_ = new ResponseViewerPanel(this);
    timeline_ = new TimelinePanel(this);

    // Right column stacks Response + Timeline in tabs (PRD §9.3 Cmd+1/2/3
    // tab switching lands with the shortcut registry later).
    auto* rightTabs = new QTabWidget(this);
    rightTabs->addTab(responseViewer_, QStringLiteral("Response"));
    rightTabs->addTab(timeline_, QStringLiteral("Timeline"));

    // Three-pane workbench (DESIGN.md §5.2): explorer | request editor |
    // response/timeline tabs. Default ratio 22 / 44 / 34; the user's drag is
    // persisted to QSettings and restored on next launch.
    mainSplitter_ = new QSplitter(Qt::Horizontal, this);
    mainSplitter_->setObjectName(QStringLiteral("mainSplitter"));
    mainSplitter_->addWidget(explorer_);
    mainSplitter_->addWidget(requestEditor_);
    mainSplitter_->addWidget(rightTabs);
    mainSplitter_->setStretchFactor(0, 22);
    mainSplitter_->setStretchFactor(1, 44);
    mainSplitter_->setStretchFactor(2, 34);
    mainSplitter_->setChildrenCollapsible(false);

    // First-run / no-project surface (DESIGN.md §10, PRD §12): teach the next
    // step instead of showing empty panels. Swapped out once a project loads.
    emptyState_ = new widgets::EmptyState(this);
    emptyState_->setTitle(QStringLiteral("No project open"));
    emptyState_->setMessage(QStringLiteral(
        "Open a ChainAPI project folder to explore its actors, resources, and operations, "
        "then run any endpoint with its full dependency chain resolved for you."));
    emptyState_->setAction(QStringLiteral("Open Project…"), [this]() { onOpenProject(); });

    rootStack_ = new QStackedWidget(this);
    rootStack_->addWidget(emptyState_);    // index 0
    rootStack_->addWidget(mainSplitter_);  // index 1
    setCentralWidget(rootStack_);
}

void MainWindow::buildMenusAndToolbar() {
    auto* openAction = new QAction(QStringLiteral("&Open Project…"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenProject);

    manageSecretsAction_ = new QAction(QStringLiteral("Manage &Secrets…"), this);
    manageSecretsAction_->setEnabled(false);
    connect(manageSecretsAction_, &QAction::triggered, this, &MainWindow::onManageSecrets);

    auto* quitAction = new QAction(QStringLiteral("&Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(openAction);
    fileMenu->addAction(manageSecretsAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    buildAppearanceMenu();

    auto* toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(openAction);
    toolbar->addAction(manageSecretsAction_);
    toolbar->addSeparator();

    auto* envLabel = new QLabel(QStringLiteral("Environment:"), this);
    envLabel->setContentsMargins(8, 0, 4, 0);
    toolbar->addWidget(envLabel);
    envCombo_ = new QComboBox(this);
    envCombo_->setMinimumWidth(160);
    envCombo_->setEnabled(false);
    envCombo_->setAccessibleName(QStringLiteral("Environment"));
    envLabel->setBuddy(envCombo_);
    toolbar->addWidget(envCombo_);

    toolbar->addSeparator();
    captureBodiesCheck_ = new QCheckBox(QStringLiteral("Capture response bodies"), this);
    captureBodiesCheck_->setToolTip(QStringLiteral(
        "Capture and store full response bodies (including auth/login responses, which carry "
        "tokens). Off by default — bodies are kept out of the run log and history unless you "
        "opt in. Captured bodies are persisted to the local history database."));
    toolbar->addWidget(captureBodiesCheck_);

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);
}

void MainWindow::buildAppearanceMenu() {
    using Mode = theming::ThemeManager::Mode;

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto* appearanceMenu = viewMenu->addMenu(QStringLiteral("&Appearance"));

    // Exclusive radio group so the active mode shows a check mark.
    auto* group = new QActionGroup(this);
    group->setExclusive(true);

    const auto addMode = [&](const QString& label, Mode mode) {
        auto* action = appearanceMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(themeManager_.mode() == mode);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode]() { themeManager_.setMode(mode); });
    };

    addMode(QStringLiteral("&Light"), Mode::Light);
    addMode(QStringLiteral("&Dark"), Mode::Dark);
    addMode(QStringLiteral("&System"), Mode::System);

    buildDensityMenu();
}

void MainWindow::buildDensityMenu() {
    // Reuse the View menu created by buildAppearanceMenu rather than adding a
    // second one.
    QMenu* targetMenu = nullptr;
    const auto menus = menuBar()->findChildren<QMenu*>();
    for (auto* menu : menus) {
        if (menu->title() == QStringLiteral("&View")) {
            targetMenu = menu;
            break;
        }
    }
    if (targetMenu == nullptr) {
        targetMenu = menuBar()->addMenu(QStringLiteral("&View"));
    }

    auto* densityMenu = targetMenu->addMenu(QStringLiteral("&Density"));
    auto* group = new QActionGroup(this);
    group->setExclusive(true);

    const auto addDensity = [&](const QString& label, Density density) {
        auto* action = densityMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(density_ == density);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, density]() {
            density_ = density;
            applyDensity(density);
            QSettings settings;
            LayoutSettings::saveDensity(settings, density);
        });
    };

    addDensity(QStringLiteral("&Comfortable"), Density::Comfortable);
    addDensity(QStringLiteral("Co&mpact"), Density::Compact);
}

void MainWindow::connectSignals() {
    connect(&project_, &ProjectModel::loaded, this, &MainWindow::onProjectLoaded);
    connect(&project_, &ProjectModel::loadFailed, this, &MainWindow::onProjectLoadFailed);

    connect(
        explorer_, &ProjectExplorerWidget::operationSelected, this, [this](const QString& opId) {
            requestEditor_->showOperation(project_, opId);
        });
    connect(
        explorer_, &ProjectExplorerWidget::operationActivated, this, [this](const QString& opId) {
            requestEditor_->showOperation(project_, opId);
            onRunRequested(opId, /*clean=*/false, /*dryRun=*/false);
        });

    connect(requestEditor_, &RequestEditorPanel::runRequested, this, &MainWindow::onRunRequested);

    connect(captureBodiesCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        runController_->setCaptureResponseBodies(checked);
    });

    // Persist the active environment per-project whenever the user changes
    // it. Skipped while restoringEnv_ is set so programmatic restores on
    // project load don't echo back to settings.
    connect(envCombo_, &QComboBox::currentTextChanged, this, [this](const QString& env) {
        if (!restoringEnv_ && !env.isEmpty()) {
            saveSelectedEnvironment(env);
        }
    });

    // Stream run events to the timeline + response panels. Native-typed
    // signal payloads queue across the worker→GUI thread boundary.
    connect(runController_, &RunController::runStarted, timeline_, &TimelinePanel::onRunStarted);
    connect(runController_, &RunController::stepStarted, timeline_, &TimelinePanel::onStepStarted);
    connect(runController_, &RunController::stepSkipped, timeline_, &TimelinePanel::onStepSkipped);
    connect(runController_,
            &RunController::requestPrepared,
            timeline_,
            &TimelinePanel::onRequestPrepared);
    connect(runController_,
            &RunController::responseReceived,
            timeline_,
            &TimelinePanel::onResponseReceived);
    connect(runController_,
            &RunController::extractionCompleted,
            timeline_,
            &TimelinePanel::onExtractionCompleted);
    connect(runController_, &RunController::stepFailed, timeline_, &TimelinePanel::onStepFailed);
    connect(runController_, &RunController::runEnded, timeline_, &TimelinePanel::onRunEnded);

    connect(runController_,
            &RunController::responseReceived,
            responseViewer_,
            &ResponseViewerPanel::onResponseReceived);

    connect(runController_, &RunController::runningChanged, this, &MainWindow::onRunningChanged);
    connect(runController_, &RunController::runFinished, this, &MainWindow::onRunFinished);

    connect(
        &themeManager_, &theming::ThemeManager::themeChanged, this, &MainWindow::onThemeChanged);
}

void MainWindow::onOpenProject() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Open ChainAPI Project"), QString{});
    if (!dir.isEmpty()) {
        openProjectDirectory(dir);
    }
}

void MainWindow::openProjectDirectory(const QString& directory) {
    if (runController_->isRunning()) {
        statusBar()->showMessage(QStringLiteral("Cannot reload while a run is in progress."), 4000);
        return;
    }
    project_.loadFromDirectory(directory);
}

void MainWindow::onManageSecrets() {
    if (!project_.hasProject()) {
        return;
    }
    SecretsDialog dialog(*secretManager_, project_, themeManager_.theme(), this);
    dialog.exec();
}

void MainWindow::onThemeChanged(const theming::Theme& theme) {
    explorer_->applyTheme(theme);
    requestEditor_->applyTheme(theme);
    responseViewer_->applyTheme(theme);
    timeline_->applyTheme(theme);
    emptyState_->setTheme(theme);
}

void MainWindow::restoreSplitterSizes() {
    QSettings settings;
    const QList<int> sizes = LayoutSettings::loadSplitter(settings, QStringLiteral("mainSplitter"));
    if (sizes.size() == mainSplitter_->count()) {
        mainSplitter_->setSizes(sizes);
    }
}

void MainWindow::persistSplitterSizes() {
    QSettings settings;
    LayoutSettings::saveSplitter(settings, QStringLiteral("mainSplitter"), mainSplitter_->sizes());
}

void MainWindow::applyDensity(Density density) {
    // Compact tightens list-row vertical padding for users with hundreds of
    // operations (DESIGN.md §5.3). Driven by a dynamic property the central
    // QSS keys on, so it's a token-consistent restyle, not per-widget hacks.
    const auto value =
        density == Density::Compact ? QStringLiteral("compact") : QStringLiteral("comfortable");
    setProperty("density", value);
    if (explorer_ != nullptr) {
        explorer_->setProperty("density", value);
    }
    // Re-polish so the QSS density selectors take effect immediately.
    if (auto* s = style()) {
        s->unpolish(this);
        s->polish(this);
    }
}

void MainWindow::onProjectLoaded() {
    explorer_->populate(project_);
    requestEditor_->clearOperation();
    responseViewer_->reset();
    timeline_->reset();

    // Swap from the first-run empty state to the workbench.
    rootStack_->setCurrentWidget(mainSplitter_);

    envCombo_->clear();
    envCombo_->addItems(project_.environmentNames());
    envCombo_->setEnabled(true);
    manageSecretsAction_->setEnabled(true);

    // Restore the environment last used for this project. Falls back to the
    // project default (which environmentNames() lists first, so an unset or
    // stale entry leaves the combo on the default). Guard the selection so
    // restoring doesn't write the value straight back via the change handler.
    restoringEnv_ = true;
    const QString savedEnv = loadSavedEnvironment();
    if (!savedEnv.isEmpty()) {
        const int idx = envCombo_->findText(savedEnv);
        if (idx >= 0) {
            envCombo_->setCurrentIndex(idx);
        }
    }
    restoringEnv_ = false;

    setWindowTitle(QStringLiteral("ChainAPI — %1").arg(project_.name()));
    statusLabel_->setText(QStringLiteral("Project: %1").arg(project_.name()));
    statusBar()->showMessage(
        QStringLiteral("Loaded %1 — select an operation and press Send.").arg(project_.name()),
        5000);
}

void MainWindow::onProjectLoadFailed(const QString& code, const QString& detail) {
    statusBar()->showMessage(QStringLiteral("Load failed [%1]: %2").arg(code, detail), 8000);
}

void MainWindow::onRunRequested(const QString& operationId, bool clean, bool dryRun) {
    if (runController_->isRunning()) {
        return;
    }
    responseViewer_->reset();
    const QString env = envCombo_->currentText();
    runController_->run(operationId, env, clean, dryRun);
}

void MainWindow::onRunningChanged(bool running) {
    requestEditor_->setRunEnabled(!running);
    envCombo_->setEnabled(!running);
    captureBodiesCheck_->setEnabled(!running);
    if (running) {
        statusBar()->showMessage(QStringLiteral("Running…"));
    }
}

void MainWindow::onRunFinished(const RunReport& report) {
    if (report.engineError) {
        statusBar()->showMessage(
            QStringLiteral("Engine error [%1]: %2").arg(report.errorCode, report.errorDetail),
            8000);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Run %1 — %2 steps")
            .arg(report.outcome == engine::RunOutcome::Succeeded ? QStringLiteral("succeeded")
                                                                 : QStringLiteral("finished"))
            .arg(report.steps.size()),
        5000);
}

QString MainWindow::loadSavedEnvironment() const {
    QSettings settings;
    return EnvironmentSettings::load(settings, project_.rootPath());
}

void MainWindow::saveSelectedEnvironment(const QString& env) {
    QSettings settings;
    EnvironmentSettings::save(settings, project_.rootPath(), env);
}

}  // namespace chainapi::desktop
