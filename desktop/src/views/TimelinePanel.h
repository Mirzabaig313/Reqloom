// Right-hand panel: the live run timeline (PRD FR-7.6) and the visible data
// flow (PRD §10.3.5). Each streamed RunEvent appends or annotates a row;
// extraction values render in green, nulls/misses in red, so subtly-wrong
// extractions become obvious without reading YAML.
#pragma once

#include "../theming/Theme.h"

#include <QtWidgets/QWidget>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace chainapi::desktop {

class TimelinePanel : public QWidget {
    Q_OBJECT

public:
    explicit TimelinePanel(QWidget* parent = nullptr);
    ~TimelinePanel() override;

    TimelinePanel(const TimelinePanel&) = delete;
    TimelinePanel& operator=(const TimelinePanel&) = delete;
    TimelinePanel(TimelinePanel&&) = delete;
    TimelinePanel& operator=(TimelinePanel&&) = delete;

public slots:
    void onRunStarted(QString target, int chainSize, QString environment);
    void onStepStarted(int index, QString op, int attempt);
    void onStepSkipped(int index, QString op, QString reason);
    void onRequestPrepared(
        int index, QString method, QString url, QString maskedHeaders, int bodySize);
    void onResponseReceived(int index, int status, QString headers, int bodySize, qint64 elapsedMs);
    void onExtractionCompleted(int index,
                               QString op,
                               QString variableName,
                               QString sourcePath,
                               QString outcome,
                               QString value);
    void onStepFailed(int index, QString op, QString code, QString detail);
    void onRunEnded(QString outcome);

    /// Adopt a new theme: re-colour existing rows from the status palette.
    void applyTheme(const theming::Theme& theme);

    /// Clear all rows (e.g. when a new run starts or a project loads).
    void reset();

private:
    /// Find (or lazily create) the top-level row for a step index.
    QTreeWidgetItem* stepRow(int index, const QString& op);

    /// Set a row's Status column to a glyph + label in the token colour, so
    /// state is never conveyed by colour alone (DESIGN.md §6.1).
    void setStatusCell(QTreeWidgetItem* row, theming::StatusToken token, const QString& label);

    QLabel* header_{nullptr};
    QTreeWidget* tree_{nullptr};
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace chainapi::desktop
