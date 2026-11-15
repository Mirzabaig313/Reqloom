// AppController — run event wiring. Every event of a run lands in the tab that started it: live in
// the visible timeline and response while that tab is shown, in its parked state while it is not.
#include "AppController.h"

#include "views/Formatting.h"

#include <QtCore/QString>

#include <utility>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] QString httpStatusToken(int status) {
    if (status >= 500) {
        return QStringLiteral("error");
    }
    return status >= 300 ? QStringLiteral("warning") : QStringLiteral("success");
}

}  // namespace

void AppController::wireRunEvents() {
    wireRunLifecycle();
    wireRunSteps();
    wireRunResponses();
    wireRunChecks();
}

void AppController::wireRunLifecycle() {
    RunController* runs = runController_.get();
    connect(runs, &RunController::runningChanged, this, [this](bool running) {
        if (running) {
            beginRunInActiveTab();
        }
        running_ = running;
        emit runningChanged();
    });
    connect(runs,
            &RunController::runStarted,
            this,
            [this](const QString& target, int chainSize, const QString& environment) {
                runRouter_.setTarget(target);
                chainStatus_.clear();
                emit chainStatusChanged();
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onRunStarted(target, chainSize, environment);
                }
            });
    connect(runs, &RunController::runEnded, this, [this](const QString& outcome) {
        if (TimelineModel* timeline = runTimeline()) {
            timeline->onRunEnded(outcome);
        }
        applyRunOutcome(outcome);
        refreshHistory();
    });
    connect(runs, &RunController::runFinished, this, &AppController::finishRun);
}

void AppController::wireRunSteps() {
    RunController* runs = runController_.get();
    const auto markStep = [this](int index, const QString& op, const QString& token) {
        runRouter_.noteStep(index, op);
        chainStatus_.insert(op, token);
        emit chainStatusChanged();
    };
    connect(runs,
            &RunController::stepStarted,
            this,
            [this, markStep](int index, const QString& op, int attempt) {
                markStep(index, op, QStringLiteral("running"));
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onStepStarted(index, op, attempt);
                }
            });
    connect(runs,
            &RunController::stepSkipped,
            this,
            [this, markStep](int index, const QString& op, const QString& reason) {
                markStep(index, op, QStringLiteral("skipped"));
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onStepSkipped(index, op, reason);
                }
            });
    connect(runs,
            &RunController::stepFailed,
            this,
            [this, markStep](int index,
                             const QString& op,
                             const QString& code,
                             const QString& detail,
                             const QVariantList& diagnostics) {
                markStep(index, op, QStringLiteral("error"));
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onStepFailed(index, op, code, detail, diagnostics);
                }
            });
    connect(runs,
            &RunController::stepBlocked,
            this,
            [this, markStep](int index, const QString& op, int blockedByIndex) {
                markStep(index, op, QStringLiteral("blocked"));
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onStepBlocked(index, op, blockedByIndex);
                }
            });
}

void AppController::wireRunResponses() {
    RunController* runs = runController_.get();
    connect(runs,
            &RunController::requestPrepared,
            this,
            [this](int index,
                   const QString& method,
                   const QString& url,
                   const QString& headers,
                   int bodySize) {
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onRequestPrepared(index, method, url, headers, bodySize);
                }
            });
    connect(runs,
            &RunController::responseReceived,
            this,
            [this](int index,
                   int status,
                   const QString& headers,
                   int bodySize,
                   qint64 elapsedMs,
                   const QString& body) {
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onResponseReceived(index, status, headers, bodySize, elapsedMs, body);
                }
                const QString op = runRouter_.stepOperation(index);
                if (!op.isEmpty()) {
                    chainStatus_.insert(op, httpStatusToken(status));
                    emit chainStatusChanged();
                }
                if (runRouter_.isTargetStep(index)) {
                    applyTargetResponse(status, headers, bodySize, elapsedMs, body);
                }
                saveCandidateExample(op, status, body);
            });
}

void AppController::wireRunChecks() {
    RunController* runs = runController_.get();
    connect(runs,
            &RunController::extractionCompleted,
            this,
            [this](int index,
                   const QString& op,
                   const QString& variable,
                   const QString& sourcePath,
                   const QString& outcome,
                   const QString& value) {
                if (TimelineModel* timeline = runTimeline()) {
                    timeline->onExtractionCompleted(
                        index, op, variable, sourcePath, outcome, value);
                }
            });
    connect(
        runs,
        &RunController::assertionCompleted,
        this,
        [this](
            int index, const QString& op, const QString& name, const QString& expr, bool passed) {
            if (TimelineModel* timeline = runTimeline()) {
                timeline->onAssertionCompleted(index, op, name, expr, passed);
            }
        });
}

void AppController::saveCandidateExample(const QString& op, int status, const QString& body) {
    if (pendingCandidateOp_.isEmpty() || op != pendingCandidateOp_) {
        return;
    }
    if (status >= 200 && status < 300 && !body.isEmpty()) {
        SavedResponse example;
        example.name = QStringLiteral("Latest (auto)");
        example.status = status;
        example.body = body;
        exampleStore_.save(pendingCandidateOp_, example);
    }
    pendingCandidateOp_.clear();
    emit variableOverridesChanged();
}

void AppController::beginRunInActiveTab() {
    runRouter_.begin(activeTabKey());
    hasResponse_ = false;
    respStatus_ = 0;
    respElapsedMs_ = 0;
    respBodySize_ = 0;
    respHeaders_.clear();
    respBody_.clear();
    responseBody_.setBody(QString{});
    runOutcome_.clear();
    shownExample_.clear();
    timeline_.reset();
    emit responseChanged();
}

void AppController::finishRun(const RunReport& report) {
    if (report.engineError) {
        if (TimelineModel* timeline = runTimeline()) {
            timeline->onRunError(report.errorCode, report.errorDetail);
        }
        applyRunOutcome(format::runOutcome(engine::RunOutcome::Failed));
        emit notify(report.errorDetail.isEmpty()
                        ? tr("Run failed: %1").arg(report.errorCode)
                        : tr("Run failed (%1): %2").arg(report.errorCode, report.errorDetail),
                    true);
    }
    if (auto gathered = runRouter_.takeHiddenTimeline()) {
        if (const int index = runTabIndex(); index >= 0) {
            tabs_.stateAt(index).timeline = std::move(*gathered);
        }
    }
    if (!pendingCandidateOp_.isEmpty()) {
        pendingCandidateOp_.clear();
        emit variableOverridesChanged();
    }
    runRouter_.end();
    emit cookiesChanged();
    // Only a run authenticates an actor, and the context is readable again only now.
    refreshActorSessions();
}

RunTabRouter::TabKey AppController::activeTabKey() const {
    if (!tabs_.valid(activeTabIndex_)) {
        return {};
    }
    const TabState& tab = tabs_.stateAt(activeTabIndex_);
    return {static_cast<int>(tab.kind), tab.id};
}

int AppController::runTabIndex() const {
    const RunTabRouter::TabKey& origin = runRouter_.origin();
    if (!origin.valid()) {
        return -1;
    }
    return tabs_.indexOf(static_cast<TabState::Kind>(origin.kind), origin.id);
}

bool AppController::runTabShown() const {
    if (!runRouter_.origin().valid()) {
        return true;
    }
    const int index = runTabIndex();
    return index >= 0 && index == activeTabIndex_;
}

TimelineModel* AppController::runTimeline() {
    const int index = runTabIndex();
    const TimelineModel::Snapshot* stored = index >= 0 ? &tabs_.stateAt(index).timeline : nullptr;
    return runRouter_.timeline(timeline_, runTabShown(), stored);
}

void AppController::applyTargetResponse(
    int status, const QString& headers, int bodySize, qint64 elapsedMs, const QString& body) {
    if (!runRouter_.inFlight() || runRouter_.discarded()) {
        return;
    }
    if (runTabShown()) {
        hasResponse_ = true;
        shownExample_.clear();
        respStatus_ = status;
        respHeaders_ = headers;
        respBodySize_ = bodySize;
        respElapsedMs_ = static_cast<int>(elapsedMs);
        respBody_ = body;
        responseBody_.setBody(body);
        emit responseChanged();
        return;
    }
    if (const int index = runTabIndex(); index >= 0) {
        TabState& tab = tabs_.stateAt(index);
        tab.hasResponse = true;
        tab.shownExample.clear();
        tab.respStatus = status;
        tab.respHeaders = headers;
        tab.respBodySize = bodySize;
        tab.respElapsedMs = static_cast<int>(elapsedMs);
        tab.respBody = body;
    }
}

void AppController::applyRunOutcome(const QString& outcome) {
    if (!runRouter_.inFlight() || runRouter_.discarded()) {
        return;
    }
    if (runTabShown()) {
        runOutcome_ = outcome;
        emit responseChanged();
    } else if (const int index = runTabIndex(); index >= 0) {
        tabs_.stateAt(index).runOutcome = outcome;
    }
}

void AppController::cancelRunIfItsTabClosed() {
    if (!runRouter_.inFlight() || runRouter_.discarded() || !runRouter_.origin().valid() ||
        runTabIndex() >= 0) {
        return;
    }
    runRouter_.discard();
    runController_->cancelRun();
    emit notify(tr("The run was cancelled because its tab was closed."), false);
}

void AppController::parkHiddenRunTimeline() {
    const int index = runTabIndex();
    if (index < 0 || index != activeTabIndex_) {
        return;
    }
    if (auto gathered = runRouter_.takeHiddenTimeline()) {
        tabs_.stateAt(index).timeline = std::move(*gathered);
    }
}

}  // namespace reqloom::desktop::qml
