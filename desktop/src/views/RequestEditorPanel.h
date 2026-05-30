// Centre panel: shows the selected operation's request definition and the
// dependency chain that will execute (PRD FR-6.1/6.2/6.3). Read-only by
// default; Override Mode reveals editable controls for the full request
// surface (method, path, query, headers, body raw/form, expected status,
// timeout, force, actor) that apply to a single one-shot run. The shell reads
// `buildOverride()` and forwards it to the RunController.
#pragma once

#include "../theming/Theme.h"

#include <chainapi/engine/Operation.h>

#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QTabWidget;

namespace chainapi::desktop {

class ProjectModel;
struct RequestOverride;

namespace widgets {
class KeyValueEditor;
}  // namespace widgets

class RequestEditorPanel : public QWidget {
    Q_OBJECT

public:
    explicit RequestEditorPanel(QWidget* parent = nullptr);
    ~RequestEditorPanel() override;

    RequestEditorPanel(const RequestEditorPanel&) = delete;
    RequestEditorPanel& operator=(const RequestEditorPanel&) = delete;
    RequestEditorPanel(RequestEditorPanel&&) = delete;
    RequestEditorPanel& operator=(RequestEditorPanel&&) = delete;

    /// Show the operation identified by `operationId` from `project`.
    void showOperation(const ProjectModel& project, const QString& operationId);

    /// Reset to the empty state (no operation selected).
    void clearOperation();

    /// Enable/disable the run buttons (e.g. while a run is in flight).
    void setRunEnabled(bool enabled);

    /// Adopt a new theme (fonts + token-derived colours).
    void applyTheme(const theming::Theme& theme);

    [[nodiscard]] QString currentOperationId() const;

    /// Whether Override Mode is on (fields editable for a one-shot run, §6.3).
    [[nodiscard]] bool overrideActive() const noexcept;

    /// Snapshot the editable controls into a RequestOverride for the next run.
    /// Only meaningful when `overrideActive()`.
    [[nodiscard]] RequestOverride buildOverride() const;

signals:
    void runRequested(QString operationId, bool clean, bool dryRun);
    /// Persist the current Override edits to the saved project (FR: in-app
    /// endpoint editing instead of hand-editing YAML).
    void saveRequested(QString operationId);

private:
    void renderChainPreview(const ProjectModel& project, const engine::OperationId& target);
    void setOverrideMode(bool on);
    /// Load the editable Override controls from operation `op` of `project`.
    void loadOverrideFields(const ProjectModel& project, const engine::Operation& op);
    /// Refresh the edit-tab labels with live counts (Postman-style "Headers 8").
    void refreshTabBadges();

    // Constructor helpers — keep the ctor under the function-length limit by
    // building each region in its own method.
    [[nodiscard]] QWidget* buildHeaderRow();
    [[nodiscard]] QWidget* buildPreviewPage();
    [[nodiscard]] QWidget* buildEditPage();
    [[nodiscard]] QWidget* buildActionRow();
    void wireConnections();

    QLabel* title_{nullptr};
    QLabel* actorLabel_{nullptr};
    QCheckBox* overrideToggle_{nullptr};
    QLabel* overrideBanner_{nullptr};
    QListWidget* chainList_{nullptr};

    // Read-only preview (shown when Override Mode is off).
    QStackedWidget* requestStack_{nullptr};
    QPlainTextEdit* headersView_{nullptr};
    QPlainTextEdit* bodyView_{nullptr};

    // Editable controls (shown when Override Mode is on).
    QComboBox* methodCombo_{nullptr};
    QLineEdit* pathEdit_{nullptr};
    QTabWidget* editTabs_{nullptr};
    QComboBox* actorCombo_{nullptr};
    QLineEdit* expectStatusEdit_{nullptr};
    QSpinBox* timeoutSpin_{nullptr};
    QCheckBox* forceCheck_{nullptr};
    widgets::KeyValueEditor* headersEditor_{nullptr};
    widgets::KeyValueEditor* queryEditor_{nullptr};
    widgets::KeyValueEditor* formEditor_{nullptr};
    QComboBox* bodyKindCombo_{nullptr};
    QStackedWidget* bodyStack_{nullptr};
    QPlainTextEdit* bodyRawEdit_{nullptr};

    QPushButton* sendButton_{nullptr};
    QPushButton* sendCleanButton_{nullptr};
    QPushButton* dryRunButton_{nullptr};
    QPushButton* saveButton_{nullptr};

    QString currentOp_;
    bool overrideActive_{false};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop
