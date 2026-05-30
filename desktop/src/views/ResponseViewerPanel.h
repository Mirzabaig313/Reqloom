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

private:
    void renderBody(const QString& body);
    void populateTree(QTreeWidgetItem* parent, const QJsonValue& value);
    void showBodyPlaceholder(const QString& message);
    /// Status colour for an HTTP status code, from the theme status palette.
    [[nodiscard]] QColor statusColor(int httpStatus) const;

    QLabel* statusLabel_{nullptr};
    QTabWidget* tabs_{nullptr};
    QTreeWidget* bodyTree_{nullptr};
    QPlainTextEdit* bodyRaw_{nullptr};
    QPlainTextEdit* headersView_{nullptr};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop
