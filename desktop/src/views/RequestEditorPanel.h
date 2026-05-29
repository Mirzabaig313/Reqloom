// Centre panel: shows the selected operation's request definition and the
// dependency chain that will execute (PRD FR-6.1/6.2/6.3). Read-only in the
// MVP — override mode lands later. The Send / Send Cleanly / Dry Run buttons
// originate the run requests the shell forwards to the RunController.
#pragma once

#include <chainapi/engine/Operation.h>

#include <QtWidgets/QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QListWidget;

namespace chainapi::desktop {

class ProjectModel;

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

    [[nodiscard]] QString currentOperationId() const;

signals:
    void runRequested(QString operationId, bool clean, bool dryRun);

private:
    void renderChainPreview(const ProjectModel& project, const engine::OperationId& target);

    QLabel* title_{nullptr};
    QLabel* actorLabel_{nullptr};
    QListWidget* chainList_{nullptr};
    QPlainTextEdit* headersView_{nullptr};
    QPlainTextEdit* bodyView_{nullptr};
    QPushButton* sendButton_{nullptr};
    QPushButton* sendCleanButton_{nullptr};
    QPushButton* dryRunButton_{nullptr};

    QString currentOp_;
};

}  // namespace chainapi::desktop
