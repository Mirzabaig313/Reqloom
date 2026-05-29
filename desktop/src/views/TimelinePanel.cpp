// TimelinePanel — see header. Live run timeline + visible data flow.
#include "TimelinePanel.h"

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop {

namespace {

// Stash the step index on a top-level row so streamed events for the same
// step annotate the existing row instead of appending a duplicate.
constexpr int kStepIndexRole = Qt::UserRole + 1;

const QColor kResolvedColor{0x2E, 0x7D, 0x32};  // green
const QColor kProblemColor{0xC6, 0x28, 0x28};   // red

}  // namespace

TimelinePanel::TimelinePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    header_ = new QLabel(QStringLiteral("Timeline"), this);
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(header_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels(
        {QStringLiteral("Step"), QStringLiteral("Status"), QStringLiteral("Detail")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(tree_, 1);
}

TimelinePanel::~TimelinePanel() = default;

void TimelinePanel::reset() {
    tree_->clear();
    header_->setText(QStringLiteral("Timeline"));
}

QTreeWidgetItem* TimelinePanel::stepRow(int index, const QString& op) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        if (item->data(0, kStepIndexRole).toInt() == index) {
            return item;
        }
    }
    auto* item = new QTreeWidgetItem(tree_);
    item->setData(0, kStepIndexRole, index);
    item->setText(0, QStringLiteral("%1. %2").arg(index + 1).arg(op));
    tree_->addTopLevelItem(item);
    return item;
}

void TimelinePanel::onRunStarted(QString target, int chainSize, QString environment) {
    reset();
    header_->setText(QStringLiteral("Running %1  ·  %2 steps  ·  env=%3")
                         .arg(target)
                         .arg(chainSize)
                         .arg(environment));
}

void TimelinePanel::onStepStarted(int index, QString op, int attempt) {
    auto* row = stepRow(index, op);
    row->setText(1, QStringLiteral("running"));
    if (attempt > 1) {
        row->setText(2, QStringLiteral("attempt %1").arg(attempt));
    }
}

void TimelinePanel::onStepSkipped(int index, QString op, QString reason) {
    auto* row = stepRow(index, op);
    row->setText(1, QStringLiteral("skipped"));
    row->setText(2, reason);
}

void TimelinePanel::onRequestPrepared(
    int index, QString method, QString url, QString maskedHeaders, int bodySize) {
    auto* row = stepRow(index, QString{});
    auto* req = new QTreeWidgetItem(row);
    req->setText(0, QStringLiteral("→ request"));
    req->setText(2, QStringLiteral("%1 %2  (%3 body bytes)").arg(method, url).arg(bodySize));
    if (!maskedHeaders.isEmpty()) {
        req->setToolTip(2, maskedHeaders);
    }
    row->setExpanded(true);
}

void TimelinePanel::onResponseReceived(
    int index, int status, QString headers, int bodySize, qint64 elapsedMs) {
    auto* row = stepRow(index, QString{});
    auto* resp = new QTreeWidgetItem(row);
    resp->setText(0, QStringLiteral("← response"));
    resp->setText(
        2,
        QStringLiteral("HTTP %1  ·  %2 bytes  ·  %3 ms").arg(status).arg(bodySize).arg(elapsedMs));
    if (!headers.isEmpty()) {
        resp->setToolTip(2, headers);
    }
}

void TimelinePanel::onExtractionCompleted(int index,
                                          QString /*op*/,
                                          QString variableName,
                                          QString sourcePath,
                                          QString outcome,
                                          QString value) {
    auto* row = stepRow(index, QString{});
    auto* ext = new QTreeWidgetItem(row);
    const bool resolved = (outcome == QStringLiteral("resolved"));
    ext->setText(0, QStringLiteral("  %1").arg(variableName));
    if (resolved) {
        ext->setText(1, QStringLiteral("="));
        ext->setText(2, value);
        ext->setForeground(2, QBrush(kResolvedColor));
    } else {
        // null / missing / invalid — the data-flow red flag (§10.3.5).
        ext->setText(1, outcome);
        ext->setText(2, QStringLiteral("%1  (%2)").arg(outcome, sourcePath));
        ext->setForeground(1, QBrush(kProblemColor));
        ext->setForeground(2, QBrush(kProblemColor));
    }
    row->setExpanded(true);
}

void TimelinePanel::onStepFailed(int index, QString op, QString code, QString detail) {
    auto* row = stepRow(index, op);
    row->setText(1, QStringLiteral("FAILED"));
    row->setText(2, QStringLiteral("[%1] %2").arg(code, detail));
    row->setForeground(1, QBrush(kProblemColor));
    row->setForeground(2, QBrush(kProblemColor));
}

void TimelinePanel::onRunEnded(QString outcome) {
    header_->setText(QStringLiteral("%1  ·  %2").arg(header_->text(), outcome));
}

}  // namespace chainapi::desktop
