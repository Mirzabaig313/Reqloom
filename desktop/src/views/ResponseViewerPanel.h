// Response panel (PRD FR-7.1/7.2/7.3). Shows the latest response's status,
// masked headers, and — when the run opted into body capture — the full
// response body as a collapsible JSON tree and a raw text view.
//
// Bodies only arrive when RunController::setCaptureResponseBodies(true) was
// set, mirroring the engine's redaction-first contract. When capture is off
// the body tabs explain why they're empty rather than showing a blank pane.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QJsonValue;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace chainapi::desktop {

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

signals:
    /// Emitted when the user clicks a tree value and its JSONPath is copied to
    /// the clipboard (FR-7.4). The shell surfaces a confirmation toast.
    void jsonPathCopied(QString path);

private:
    void renderBody(const QString& body);
    void renderDiff(const QString& previousBody, const QString& currentBody);
    void populateTree(QTreeWidgetItem* parent, const QString& path, const QJsonValue& value);
    void showBodyPlaceholder(const QString& message);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    /// Status colour for an HTTP status code, from the theme status palette.
    [[nodiscard]] QColor statusColor(int httpStatus) const;

    QLabel* statusLabel_{nullptr};
    QTabWidget* tabs_{nullptr};
    QTreeWidget* bodyTree_{nullptr};
    QPlainTextEdit* bodyRaw_{nullptr};
    QPlainTextEdit* headersView_{nullptr};
    QTextEdit* diffView_{nullptr};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
    // Last HTTP status shown, so a runtime theme switch can re-resolve the
    // status-label colour (-1 = nothing shown yet).
    int lastStatus_{-1};
    // The body of the previous response (pretty-printed) so the Diff tab can
    // compare the current one against it. Empty until two bodies have arrived.
    QString previousBody_;
    QString currentBody_;
};

}  // namespace chainapi::desktop
