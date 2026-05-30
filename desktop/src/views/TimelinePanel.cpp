// TimelinePanel — see header. Live run timeline + visible data flow.
#include "TimelinePanel.h"

#include "../widgets/StatusBadge.h"

#include <QtGui/QBrush>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <functional>

namespace chainapi::desktop {

namespace {

// Stash the step index on a top-level row so streamed events for the same
// step annotate the existing row instead of appending a duplicate.
constexpr int kStepIndexRole = Qt::UserRole + 1;
// The status token a row's column-1 colour was derived from, so a runtime
// theme switch can re-resolve the colour rather than leave it stale.
constexpr int kStatusTokenRole = Qt::UserRole + 2;
// Marks column 2 as carrying error-coloured detail text (failed steps), so it
// too re-colours on a theme switch.
constexpr int kDetailErrorRole = Qt::UserRole + 3;

}  // namespace

TimelinePanel::TimelinePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    const int gap = theming::Theme::space(theming::Space::Sm);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(gap);

    header_ = new QLabel(QStringLiteral("Timeline"), this);
    header_->setFont(theme_.font(theming::TextStyle::Subtitle));
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

void TimelinePanel::applyTheme(const theming::Theme& theme) {
    theme_ = theme;
    header_->setFont(theme_.font(theming::TextStyle::Subtitle));
    // Re-resolve every row's status colour from the stored token so an
    // already-rendered timeline restyles on a runtime Light/Dark switch.
    const std::function<void(QTreeWidgetItem*)> recolor = [&](QTreeWidgetItem* item) {
        const QVariant token = item->data(0, kStatusTokenRole);
        if (token.isValid()) {
            const auto t = static_cast<theming::StatusToken>(token.toInt());
            item->setForeground(1, QBrush(theme_.status(t)));
        }
        if (item->data(0, kDetailErrorRole).toBool()) {
            item->setForeground(2, QBrush(theme_.status(theming::StatusToken::Error)));
        }
        for (int i = 0; i < item->childCount(); ++i) {
            recolor(item->child(i));
        }
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        recolor(tree_->topLevelItem(i));
    }
}

void TimelinePanel::reset() {
    tree_->clear();
    header_->setText(QStringLiteral("Timeline"));
}

void TimelinePanel::setStatusCell(QTreeWidgetItem* row,
                                  theming::StatusToken token,
                                  const QString& label) {
    const QString glyph = widgets::StatusBadge::glyph(token);
    row->setText(1, QStringLiteral("%1 %2").arg(glyph, label));
    row->setForeground(1, QBrush(theme_.status(token)));
    row->setData(0, kStatusTokenRole, static_cast<int>(token));
}

QTreeWidgetItem* TimelinePanel::stepRow(int index, const QString& op) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i);
        if (item->data(0, kStepIndexRole).toInt() == index) {
            return item;
        }
    }
    // Constructing with `tree_` as parent already inserts the row as a
    // top-level item — no explicit addTopLevelItem needed.
    auto* item = new QTreeWidgetItem(tree_);
    item->setData(0, kStepIndexRole, index);
    item->setText(0, QStringLiteral("%1. %2").arg(index + 1).arg(op));
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
    setStatusCell(row, theming::StatusToken::Running, QStringLiteral("running"));
    if (attempt > 1) {
        row->setText(2, QStringLiteral("attempt %1").arg(attempt));
    }
}

void TimelinePanel::onStepSkipped(int index, QString op, QString reason) {
    auto* row = stepRow(index, op);
    setStatusCell(row, theming::StatusToken::Skipped, QStringLiteral("skipped"));
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
    // Settle the parent row to a terminal status by HTTP class (§2.5).
    const auto token = status >= 500   ? theming::StatusToken::Error
                       : status >= 300 ? theming::StatusToken::Warning
                                       : theming::StatusToken::Success;
    setStatusCell(row, token, QStringLiteral("HTTP %1").arg(status));

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
        ext->setForeground(2, QBrush(theme_.status(theming::StatusToken::Success)));
    } else {
        // null / missing / invalid is a non-error condition that still demands
        // attention — DESIGN.md §2.5 reserves status.warning (amber) for it.
        const QBrush warn{theme_.status(theming::StatusToken::Warning)};
        ext->setText(1, outcome);
        ext->setText(2, QStringLiteral("%1  (%2)").arg(outcome, sourcePath));
        ext->setForeground(1, warn);
        ext->setForeground(2, warn);
    }
    row->setExpanded(true);
}

void TimelinePanel::onStepFailed(int index, QString op, QString code, QString detail) {
    auto* row = stepRow(index, op);
    setStatusCell(row, theming::StatusToken::Error, QStringLiteral("failed"));
    row->setText(2, QStringLiteral("[%1] %2").arg(code, detail));
    row->setForeground(2, QBrush(theme_.status(theming::StatusToken::Error)));
    row->setData(0, kDetailErrorRole, true);
}

void TimelinePanel::onRunEnded(QString outcome) {
    header_->setText(QStringLiteral("%1  ·  %2").arg(header_->text(), outcome));
}

}  // namespace chainapi::desktop
