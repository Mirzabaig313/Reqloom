// TimelineModel — see header.
#include "TimelineModel.h"

#include "widgets/LatencyStats.h"

#include <reqloom/engine/ErrorCodes.h>

#include <QtCore/QTime>
#include <QtCore/QUrl>

#include <utility>

namespace reqloom::desktop::qml {

namespace {

/// Format a byte count for the size column (B / KB / MB), one decimal under 10.
[[nodiscard]] QString formatBytes(int bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(QString::number(kb, 'f', kb < 10.0 ? 1 : 0));
    }
    const double mb = kb / 1024.0;
    return QStringLiteral("%1 MB").arg(QString::number(mb, 'f', mb < 10.0 ? 1 : 0));
}

[[nodiscard]] QString nowClock() {
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

[[nodiscard]] QString kindToken(TimelineModel::Kind kind) {
    switch (kind) {
        case TimelineModel::Kind::RunStart:
            return QStringLiteral("runStart");
        case TimelineModel::Kind::Step:
            return QStringLiteral("step");
        case TimelineModel::Kind::Request:
            return QStringLiteral("request");
        case TimelineModel::Kind::Response:
            return QStringLiteral("response");
        case TimelineModel::Kind::Extraction:
            return QStringLiteral("extraction");
        case TimelineModel::Kind::Assertion:
            return QStringLiteral("assertion");
        case TimelineModel::Kind::Skip:
            return QStringLiteral("skip");
        case TimelineModel::Kind::Fail:
            return QStringLiteral("fail");
        case TimelineModel::Kind::RunEnd:
            return QStringLiteral("runEnd");
    }
    return QStringLiteral("step");
}

/// Turn a raw engine error code into an actionable phrase: known codes use the engine's `humanize`
/// (shared with the CLI), others are title-cased (strip E_, underscores → spaces).
[[nodiscard]] QString humanizeError(const QString& code) {
    if (const auto known = engine::fromCodeString(code.toStdString())) {
        const auto phrase = engine::humanize(*known);
        return QString::fromUtf8(phrase.data(), static_cast<qsizetype>(phrase.size()));
    }
    QString token = code;
    if (token.startsWith(QLatin1String("E_"))) {
        token.remove(0, 2);
    }
    token.replace(QLatin1Char('_'), QLatin1Char(' '));
    token = token.toLower();
    if (!token.isEmpty()) {
        token[0] = token[0].toUpper();
    }
    return token.isEmpty() ? QStringLiteral("Failed") : token;
}

void addRequestAction(QVariantMap& item) {
    const QString useKind = item.value(QStringLiteral("useKind")).toString();
    const QString useName = item.value(QStringLiteral("useName")).toString();
    QString field;
    if (useKind == QLatin1String("urlPath") || useKind == QLatin1String("rawQuery") ||
        useKind == QLatin1String("fragment")) {
        field = QStringLiteral("path");
    } else if (useKind == QLatin1String("namedQuery") && !useName.isEmpty()) {
        field = QStringLiteral("query");
    } else if (useKind == QLatin1String("header") && !useName.isEmpty()) {
        field = QStringLiteral("header");
    } else if (useKind == QLatin1String("body")) {
        field = QStringLiteral("body");
    } else if (useKind == QLatin1String("formField") && !useName.isEmpty()) {
        field = QStringLiteral("form");
    }
    item.insert(QStringLiteral("canOpenRequestField"), !field.isEmpty());
    item.insert(QStringLiteral("requestField"), field);
    item.insert(QStringLiteral("requestKey"), useName);
}

void addSourceAction(QVariantMap& item) {
    const QString kind = item.value(QStringLiteral("sourceKind")).toString();
    const QString sourceId = item.value(QStringLiteral("sourceId")).toString();
    const QString field = item.value(QStringLiteral("sourceField")).toString();
    const QString producer = item.value(QStringLiteral("producerOperationId")).toString();
    QString editKind;
    QString editId;
    if (kind == QLatin1String("environment") && !sourceId.isEmpty() && !field.isEmpty()) {
        editKind = kind;
        editId = sourceId;
    } else if (kind == QLatin1String("secret") && !field.isEmpty()) {
        editKind = kind;
        editId = field;
    } else if (kind == QLatin1String("actor") && !sourceId.isEmpty()) {
        editKind = kind;
        editId = sourceId;
    } else if (kind == QLatin1String("extraction") && !producer.isEmpty() && !field.isEmpty()) {
        editKind = kind;
        editId = field;
    }
    item.insert(QStringLiteral("canEditSource"), !editKind.isEmpty());
    item.insert(QStringLiteral("editKind"), editKind);
    item.insert(QStringLiteral("editId"), editId);
    item.insert(QStringLiteral("editField"), field);
    item.insert(QStringLiteral("editOperationId"), producer);
}

}  // namespace

TimelineModel::TimelineModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TimelineModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case KindRole:
            return kindToken(row.kind);
        case IndexRole:
            return row.stepIndex;
        case TitleRole:
            return row.title;
        case DetailRole:
            return row.detail;
        case StatusTokenRole:
            return row.statusToken;
        case StatusLabelRole:
            return row.statusLabel;
        case ValueRole:
            return row.value;
        case MethodRole:
            return row.method;
        case PathRole:
            return row.path;
        case SizeRole:
            return row.sizeText;
        case ClockRole:
            return row.clockText;
        case DurationRole:
            return row.durationText;
        case SubLabelRole:
            return row.subLabel;
        case OpRole:
            return row.op;
        case VariableNameRole:
            return row.variableName;
        case DiagnosticsRole:
            return row.diagnostics;
        case BlockedByStepRole:
            return row.blockedByStep;
        case RootFailureRole:
            return row.rootFailure;
        default:
            return {};
    }
}

QHash<int, QByteArray> TimelineModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {IndexRole, "stepIndex"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {StatusTokenRole, "statusToken"},
        {StatusLabelRole, "statusLabel"},
        {ValueRole, "value"},
        {MethodRole, "method"},
        {PathRole, "path"},
        {SizeRole, "size"},
        {ClockRole, "clock"},
        {DurationRole, "duration"},
        {SubLabelRole, "subLabel"},
        {OpRole, "op"},
        {VariableNameRole, "variableName"},
        {DiagnosticsRole, "diagnostics"},
        {BlockedByStepRole, "blockedByStep"},
        {RootFailureRole, "rootFailure"},
    };
}

void TimelineModel::appendRow(Row row) {
    const int at = static_cast<int>(rows_.size());
    beginInsertRows({}, at, at);
    rows_.push_back(std::move(row));
    endInsertRows();
}

int TimelineModel::stepRowFor(int index, const QString& op) {
    const auto it = stepRowByIndex_.constFind(index);
    if (it != stepRowByIndex_.constEnd()) {
        return it.value();
    }
    Row row;
    row.kind = Kind::Step;
    row.stepIndex = index + 1;
    row.title = QStringLiteral("%1. %2").arg(index + 1).arg(op);
    // Kept as its own field: `title` is a display string ("3. auth.login") and callers need the
    // bare id.
    row.op = op;
    const int at = static_cast<int>(rows_.size());
    beginInsertRows({}, at, at);
    rows_.push_back(std::move(row));
    endInsertRows();
    stepRowByIndex_.insert(index, at);
    return at;
}

QVariantList TimelineModel::prepareDiagnostics(QVariantList diagnostics) const {
    for (QVariant& value : diagnostics) {
        QVariantMap item = value.toMap();
        addRequestAction(item);
        addSourceAction(item);
        const QString producer = item.value(QStringLiteral("producerOperationId")).toString();
        const int producerStep = item.value(QStringLiteral("producerStep")).toInt();
        const bool producerMatches =
            producerStep > 0 && !producer.isEmpty() && stepForOperation(producer) == producerStep;
        item.insert(QStringLiteral("canShowProducer"), producerMatches);
        value = item;
    }
    return diagnostics;
}

void TimelineModel::reset() {
    beginResetModel();
    rows_.clear();
    stepRowByIndex_.clear();
    stepChildSeq_.clear();
    stepMs_.clear();
    runTotalMs_ = 0.0;
    runChainSize_ = 0;
    runEnv_.clear();
    runStartRow_ = -1;
    const bool hadRootFailure = rootFailureRow_ >= 0;
    rootFailureRow_ = -1;
    endResetModel();
    if (hadRootFailure) {
        emit rootFailureChanged();
    }
    if (!latencyMs_.empty() || !latencyBars_.isEmpty()) {
        latencyMs_.clear();
        latencyBars_.clear();
        emit latenciesChanged();
    }
    // A pinned step cannot outlive its rows; drop the pin so the inspector follows the new run.
    if (selectedStep_ != 0) {
        selectedStep_ = 0;
        emit selectionChanged();
    }
}

int TimelineModel::stepForOperation(const QString& operationId) const {
    if (operationId.isEmpty()) {
        return 0;
    }
    for (const Row& row : rows_) {
        if (row.kind == Kind::Step && row.op == operationId) {
            return row.stepIndex;
        }
    }
    return 0;
}

void TimelineModel::setSelectedStep(int stepNumber) {
    // Only a step with a row may be pinned: a stale click or old-run bar would otherwise pin a dead
    // step.
    const int next = (stepNumber > 0 && rowForStep(stepNumber) >= 0) ? stepNumber : 0;
    if (next == selectedStep_) {
        return;
    }
    selectedStep_ = next;
    emit selectionChanged();
}

QString TimelineModel::selectedOperationId() const {
    const int at = rowForStep(selectedStep_);
    if (at < 0 || at >= static_cast<int>(rows_.size())) {
        return {};
    }
    return rows_[static_cast<std::size_t>(at)].op;
}

TimelineModel::Snapshot TimelineModel::takeSnapshot() const {
    return Snapshot{.rows = rows_,
                    .latencyMs = latencyMs_,
                    .latencyBars = latencyBars_,
                    .stepRowByIndex = stepRowByIndex_,
                    .stepChildSeq = stepChildSeq_,
                    .stepMs = stepMs_,
                    .runTotalMs = runTotalMs_,
                    .runChainSize = runChainSize_,
                    .runEnv = runEnv_,
                    .runStartRow = runStartRow_,
                    .rootFailureRow = rootFailureRow_,
                    .selectedStep = selectedStep_};
}

void TimelineModel::restoreSnapshot(Snapshot snapshot) {
    beginResetModel();
    rows_ = std::move(snapshot.rows);
    stepRowByIndex_ = std::move(snapshot.stepRowByIndex);
    stepChildSeq_ = std::move(snapshot.stepChildSeq);
    stepMs_ = std::move(snapshot.stepMs);
    runTotalMs_ = snapshot.runTotalMs;
    runChainSize_ = snapshot.runChainSize;
    runEnv_ = std::move(snapshot.runEnv);
    runStartRow_ = snapshot.runStartRow;
    rootFailureRow_ = snapshot.rootFailureRow;
    // Swap latency data in before endResetModel so a view never sees the previous tab's bars
    // against new rows.
    latencyMs_ = std::move(snapshot.latencyMs);
    latencyBars_ = std::move(snapshot.latencyBars);
    selectedStep_ = snapshot.selectedStep;
    endResetModel();
    emit latenciesChanged();
    emit rootFailureChanged();
    // Unconditional: the step number may be unchanged while the rows under it belong to a different
    // tab.
    emit selectionChanged();
}

int TimelineModel::rowForStep(int stepNumber) const {
    // Bars carry the 1-based step number; stepRowByIndex_ is keyed by the 0-based event index.
    return stepRowByIndex_.value(stepNumber - 1, -1);
}

QVariantMap TimelineModel::latencyStats() const {
    QVariantMap out;
    const stats::Summary s = stats::summarize(latencyMs_);
    out.insert(QStringLiteral("count"), static_cast<int>(s.count));
    out.insert(QStringLiteral("min"), s.min);
    out.insert(QStringLiteral("max"), s.max);
    out.insert(QStringLiteral("mean"), s.mean);
    out.insert(QStringLiteral("median"), s.median);
    out.insert(QStringLiteral("p95"), s.p95);
    out.insert(QStringLiteral("p99"), s.p99);

    const stats::Histogram h = stats::histogram(latencyMs_);
    out.insert(QStringLiteral("start"), h.start);
    out.insert(QStringLiteral("binWidth"), h.binWidth);
    QVariantList bins;
    bins.reserve(static_cast<qsizetype>(h.counts.size()));
    for (const int c : h.counts) {
        bins.append(c);
    }
    out.insert(QStringLiteral("bins"), bins);
    return out;
}

void TimelineModel::onRunStarted(QString target, int chainSize, QString environment) {
    reset();
    runChainSize_ = chainSize;
    runEnv_ = environment;
    Row row;
    row.kind = Kind::RunStart;
    row.title = target;
    row.detail = QStringLiteral("%1 steps  ·  env=%2").arg(chainSize).arg(environment);
    row.statusToken = QStringLiteral("running");
    row.statusLabel = QStringLiteral("running");
    runStartRow_ = static_cast<int>(rows_.size());
    appendRow(std::move(row));
}

void TimelineModel::onStepStarted(int index, QString op, int attempt) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("running");
    row.statusLabel = QStringLiteral("running");
    if (attempt > 1) {
        row.detail = QStringLiteral("attempt %1").arg(attempt);
    }
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onStepSkipped(int index, QString op, QString reason) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("skipped");
    row.statusLabel = QStringLiteral("skipped");
    row.detail = reason;
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onRequestPrepared(
    int index, QString method, QString url, QString maskedHeaders, int bodySize) {
    Row row;
    row.kind = Kind::Request;
    row.stepIndex = index + 1;
    row.subLabel = QStringLiteral("%1.%2").arg(index + 1).arg(++stepChildSeq_[index]);
    row.title = QStringLiteral("\u2192 Request");  // → Request
    // Keep the summary short so it never elides; the resolved URL is in the expansion.
    row.detail = QStringLiteral("%1  ·  %2 body bytes").arg(method).arg(bodySize);
    row.statusToken = QStringLiteral("neutral");
    row.method = method;
    const QString path = QUrl(url).path();
    row.path = path.isEmpty() ? url : path;
    row.sizeText = formatBytes(bodySize);
    row.clockText = nowClock();
    // Expansion: the fully-resolved URL, then the masked request headers.
    QString expanded = QStringLiteral("URL\n%1").arg(url);
    if (!maskedHeaders.isEmpty()) {
        expanded += QStringLiteral("\n\nHeaders\n%1").arg(maskedHeaders);
    }
    row.value = expanded;
    appendRow(std::move(row));
}

void TimelineModel::onResponseReceived(
    int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString body) {
    // Settle the parent step row to a terminal status by HTTP class and accumulate its response
    // time.
    stepMs_[index] += static_cast<double>(elapsedMs);
    runTotalMs_ += static_cast<double>(elapsedMs);
    const auto it = stepRowByIndex_.constFind(index);
    if (it != stepRowByIndex_.constEnd()) {
        Row& step = rows_[static_cast<std::size_t>(it.value())];
        step.statusToken = status >= 500   ? QStringLiteral("error")
                           : status >= 300 ? QStringLiteral("warning")
                                           : QStringLiteral("success");
        step.statusLabel = QStringLiteral("HTTP %1").arg(status);
        step.durationText = QStringLiteral("%1 ms").arg(static_cast<qint64>(stepMs_[index]));
        const QModelIndex idx = rowIndex(it.value());
        emit dataChanged(idx, idx);
    }

    Row row;
    row.kind = Kind::Response;
    row.stepIndex = index + 1;
    row.subLabel = QStringLiteral("%1.%2").arg(index + 1).arg(++stepChildSeq_[index]);
    row.title = QStringLiteral("\u2190 Response");  // ← Response
    row.detail =
        QStringLiteral("HTTP %1  ·  %2 bytes  ·  %3 ms").arg(status).arg(bodySize).arg(elapsedMs);
    row.statusToken = status >= 500   ? QStringLiteral("error")
                      : status >= 300 ? QStringLiteral("warning")
                                      : QStringLiteral("success");
    row.statusLabel = QStringLiteral("HTTP %1").arg(status);
    row.sizeText = formatBytes(bodySize);
    row.durationText = QStringLiteral("%1 ms").arg(elapsedMs);
    row.clockText = nowClock();
    // Expansion: status line, headers, and the body when the run captured it.
    QString expanded = QStringLiteral("Status\nHTTP %1").arg(status);
    if (!headers.isEmpty()) {
        expanded += QStringLiteral("\n\nHeaders\n%1").arg(headers);
    }
    if (!body.isEmpty()) {
        expanded += QStringLiteral("\n\nBody\n%1").arg(body);
    }
    row.value = expanded;
    appendRow(std::move(row));

    const QString token = status >= 500   ? QStringLiteral("error")
                          : status >= 300 ? QStringLiteral("warning")
                                          : QStringLiteral("success");
    latencyMs_.push_back(static_cast<double>(elapsedMs));
    QVariantMap bar;
    bar.insert(QStringLiteral("ms"), static_cast<double>(elapsedMs));
    bar.insert(QStringLiteral("token"), token);
    bar.insert(QStringLiteral("op"), QStringLiteral("step %1").arg(index + 1));
    // 1-based step number, so clicking this bar scrolls to the owning step row.
    bar.insert(QStringLiteral("stepIndex"), index + 1);
    latencyBars_.append(bar);
    emit latenciesChanged();
}

void TimelineModel::onExtractionCompleted(int index,
                                          QString op,
                                          QString variableName,
                                          QString sourcePath,
                                          QString outcome,
                                          QString value) {
    Row row;
    row.kind = Kind::Extraction;
    row.stepIndex = index + 1;
    row.title = variableName;
    // Producing op + bare variable name identify which downstream steps consumed this value.
    row.op = op;
    row.variableName = variableName;
    const bool resolved = (outcome == QLatin1String("resolved"));
    if (resolved) {
        row.statusToken = QStringLiteral("success");
        row.statusLabel = QStringLiteral("=");
        row.detail = value;
        row.value = value;
    } else {
        // A null/missing/invalid extraction is a non-error that still needs attention: amber, never
        // red.
        row.statusToken = QStringLiteral("warning");
        row.statusLabel = outcome;
        row.detail = QStringLiteral("%1  (%2)").arg(outcome, sourcePath);
        row.value = sourcePath;
    }
    appendRow(std::move(row));
}

void TimelineModel::onAssertionCompleted(
    int index, QString /*op*/, QString name, QString expr, bool passed) {
    Row row;
    row.kind = Kind::Assertion;
    row.stepIndex = index + 1;
    row.title = name;
    // A failed assertion also fails the step, so it is red (unlike a null extraction, which is
    // amber).
    row.statusToken = passed ? QStringLiteral("success") : QStringLiteral("error");
    row.statusLabel = passed ? QStringLiteral("\u2713") : QStringLiteral("\u2717");
    row.detail = expr;
    row.value = expr;
    appendRow(std::move(row));
}

void TimelineModel::onStepFailed(
    int index, QString op, QString code, QString detail, QVariantList diagnostics) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("error");
    row.statusLabel = QStringLiteral("failed");
    const QString friendly = humanizeError(code);
    row.detail = detail.isEmpty() ? friendly : QStringLiteral("%1 — %2").arg(friendly, detail);
    // Expansion: the full failure reason (often too long for the row) plus the raw code for bug
    // reports.
    QString expanded;
    if (!detail.isEmpty()) {
        expanded = detail + QStringLiteral("\n\n");
    }
    expanded += QStringLiteral("Error code: %1").arg(code);
    row.value = expanded;
    row.diagnostics = prepareDiagnostics(std::move(diagnostics));
    const bool firstFailure = rootFailureRow_ < 0;
    if (firstFailure) {
        rootFailureRow_ = at;
        row.rootFailure = true;
    }
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
    if (firstFailure) {
        emit rootFailureChanged();
    }
}

void TimelineModel::onStepBlocked(int index, QString op, int blockedByIndex) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("blocked");
    row.statusLabel = QStringLiteral("blocked");
    row.blockedByStep = blockedByIndex + 1;
    row.detail = QStringLiteral("Blocked by step %1").arg(row.blockedByStep);
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onRunEnded(QString outcome) {
    QString token;
    QString summary;
    QString badge;
    if (outcome.contains(QLatin1String("Succeeded"))) {
        token = QStringLiteral("success");
        summary = QStringLiteral("Run finished");
        badge = QStringLiteral("done");
    } else if (outcome.contains(QLatin1String("Cancelled"))) {
        token = QStringLiteral("cancelled");
        summary = QStringLiteral("Run cancelled");
        badge = QStringLiteral("cancelled");
    } else if (outcome.contains(QLatin1String("Failed"))) {
        token = QStringLiteral("error");
        summary = QStringLiteral("Run failed");
        badge = QStringLiteral("failed");
    } else {
        token = QStringLiteral("neutral");
        summary = outcome;
        badge = outcome;
    }

    // Settle the run header's "running" badge to the final state and fold the total response time
    // into its detail.
    if (runStartRow_ >= 0 && runStartRow_ < static_cast<int>(rows_.size())) {
        Row& header = rows_[static_cast<std::size_t>(runStartRow_)];
        header.statusToken = token;
        header.statusLabel = badge;
        header.detail = QStringLiteral("%1 steps  ·  Total time %2 ms  ·  env=%3")
                            .arg(runChainSize_)
                            .arg(static_cast<qint64>(runTotalMs_))
                            .arg(runEnv_);
        const QModelIndex idx = rowIndex(runStartRow_);
        emit dataChanged(idx, idx);
    }

    // Summary row: one phrase coloured by token; the header already carries the badge.
    Row row;
    row.kind = Kind::RunEnd;
    row.title = summary;
    row.statusToken = token;
    appendRow(std::move(row));
}

void TimelineModel::onRunError(QString code, QString detail) {
    if (runStartRow_ >= 0 && runStartRow_ < static_cast<int>(rows_.size()) &&
        rows_[static_cast<std::size_t>(runStartRow_)].statusToken == QLatin1String("running")) {
        onRunEnded(QStringLiteral("Failed"));
    }
    Row row;
    row.kind = Kind::Fail;
    row.title = humanizeError(code);
    row.detail = detail;
    row.statusToken = QStringLiteral("error");
    row.statusLabel = QStringLiteral("failed");
    row.value = detail.isEmpty() ? QStringLiteral("Error code: %1").arg(code)
                                 : QStringLiteral("%1\n\nError code: %2").arg(detail, code);
    const bool firstFailure = rootFailureRow_ < 0;
    if (firstFailure) {
        row.rootFailure = true;
        rootFailureRow_ = static_cast<int>(rows_.size());
    }
    appendRow(std::move(row));
    if (firstFailure) {
        emit rootFailureChanged();
    }
}

}  // namespace reqloom::desktop::qml
