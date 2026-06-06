// Response panel (PRD FR-7.1/7.2/7.3). Shows the latest response's status,
// masked headers, and — when the run opted into body capture — the full
// response body as a collapsible JSON tree and a raw text view.
//
// Bodies only arrive when RunController::setCaptureResponseBodies(true) was
// set, mirroring the engine's redaction-first contract. When capture is off
// the body tabs explain why they're empty rather than showing a blank pane.
#pragma once

#include "../application/SavedResponseStore.h"
#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QJsonValue;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace reqloom::desktop {

namespace widgets {
class EmptyState;
}  // namespace widgets

class ResponseViewerPanel : public QWidget {
    Q_OBJECT

public:
    explicit ResponseViewerPanel(QWidget* parent = nullptr);
    ~ResponseViewerPanel() override;

    ResponseViewerPanel(const ResponseViewerPanel&) = delete;
    ResponseViewerPanel& operator=(const ResponseViewerPanel&) = delete;
    ResponseViewerPanel(ResponseViewerPanel&&) = delete;
    ResponseViewerPanel& operator=(ResponseViewerPanel&&) = delete;

public slots:
    void onResponseReceived(
        int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString body);
    void applyTheme(const theming::Theme& theme);
    void reset();

    /// Drop the stored response-body history that backs the Diff tab. Called
    /// when a different project loads so diffs don't compare across projects.
    void clearHistory();

    /// Render a saved example (status/headers/body) without a network run.
    void showSavedResponse(const SavedResponse& example);

    /// Whether a response is currently shown (so Save can be enabled).
    [[nodiscard]] bool hasResponse() const noexcept;

    /// The currently displayed response, for saving as an example. The `name`
    /// is left empty for the caller to fill.
    [[nodiscard]] SavedResponse currentResponse() const;

signals:
    /// Emitted when the user clicks a tree value and its JSONPath is copied to
    /// the clipboard (FR-7.4). The shell surfaces a confirmation toast.
    void jsonPathCopied(QString path);

    /// The user clicked "Save" to persist the current response as an example.
    void saveResponseRequested();

private:
    void renderBody(const QString& body);
    void renderDiff(const QString& previousBody, const QString& currentBody);
    void populateTree(QTreeWidgetItem* parent, const QString& path, const QJsonValue& value);
    void showBodyPlaceholder(const QString& message);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    /// Status colour for an HTTP status code, from the theme status palette.
    [[nodiscard]] QColor statusColor(int httpStatus) const;

    QLabel* statusLabel_{nullptr};
    QPushButton* saveButton_{nullptr};
    QStackedWidget* viewStack_{nullptr};
    widgets::EmptyState* emptyState_{nullptr};
    QTabWidget* tabs_{nullptr};
    QTreeWidget* bodyTree_{nullptr};
    QPlainTextEdit* bodyRaw_{nullptr};
    QPlainTextEdit* headersView_{nullptr};
    QTextEdit* diffView_{nullptr};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
    // Last HTTP status shown, so a runtime theme switch can re-resolve the
    // status-label colour (-1 = nothing shown yet).
    int lastStatus_{-1};
    // The currently displayed response, captured so it can be saved as an
    // example. headers/body mirror what's on screen.
    QString currentHeaders_;
    QString currentRawBody_;
    int currentBodySize_{0};
    qint64 currentElapsedMs_{0};
    // The body of the previous response (pretty-printed) so the Diff tab can
    // compare the current one against it. Empty until two bodies have arrived.
    QString previousBody_;
    QString currentBody_;
};

}  // namespace reqloom::desktop
