// RunController — see header. Off-thread engine execution + event marshalling.
#include "RunController.h"

#include "../views/Formatting.h"
#include "ProjectModel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <type_traits>
#include <utility>
#include <variant>

namespace chainapi::desktop {

namespace {

namespace ce = chainapi::engine;

/// Roll a header pair list into a single newline-joined string for display.
[[nodiscard]] QString joinHeaders(const std::vector<std::pair<std::string, std::string>>& headers) {
    QString out;
    for (const auto& [key, value] : headers) {
        if (!out.isEmpty()) {
            out.append(QLatin1Char('\n'));
        }
        out.append(QString::fromStdString(key));
        out.append(QStringLiteral(": "));
        out.append(QString::fromStdString(value));
    }
    return out;
}

}  // namespace

RunController::RunController(ce::ExecutionEngine& engine,
                             const ProjectModel& project,
                             QObject* parent)
    : QObject(parent), engine_(engine), project_(project) {
    qRegisterMetaType<RunReport>("RunReport");

    // The engine retains this callback for its lifetime (no unsubscribe in the
    // public API). The callback fires on the worker thread; it only emits
    // signals, which queue to the GUI thread. RunController is owned for the
    // app's lifetime and outlives no run, so `this` stays valid here.
    engine_.subscribe([this](const ce::RunEvent& event) { publishEvent(event); });

    connect(&watcher_, &QFutureWatcher<RunReport>::finished, this, [this]() {
        const RunReport report = watcher_.result();
        running_ = false;
        emit runningChanged(false);
        emit runFinished(report);
    });
}

RunController::~RunController() {
    if (watcher_.isRunning()) {
        watcher_.waitForFinished();
    }
}

bool RunController::isRunning() const noexcept {
    return running_;
}

void RunController::resetCaches() {
    if (running_) {
        return;
    }
    if (context_) {
        context_->clearExtractions();
        if (project_.hasProject()) {
            for (const auto& [actorId, _] : project_.project().actors) {
                context_->invalidateSession(actorId);
            }
        }
    }
}

void RunController::setCaptureResponseBodies(bool capture) noexcept {
    captureResponseBodies_ = capture;
}

bool RunController::captureResponseBodies() const noexcept {
    return captureResponseBodies_;
}

void RunController::run(const QString& target,
                        const QString& environment,
                        bool clean,
                        bool dryRun) {
    if (running_ || !project_.hasProject()) {
        return;
    }

    if (!context_) {
        context_ = std::make_unique<ce::RunContext>();
    }

    ce::RunOptions options;
    options.environment = environment.toStdString();
    options.dryRun = dryRun;
    options.resetExtractions = clean;
    options.resetSessions = clean;
    options.captureResponseBodies = captureResponseBodies_;

    const ce::OperationId targetId{target.toStdString()};
    const ce::Project& project = project_.project();
    ce::RunContext& ctx = *context_;

    running_ = true;
    emit runningChanged(true);

    // engine_.run blocks; run it on a worker. The captured references
    // (engine, project, ctx) outlive the run: the engine lives for the app
    // lifetime, the project is pinned while a run is active, and ctx is a
    // member owned by this controller.
    auto future = QtConcurrent::run([this, &project, targetId, &ctx, options]() -> RunReport {
        RunReport report;
        auto result = engine_.run(project, targetId, ctx, options);
        if (!result) {
            report.engineError = true;
            report.errorCode = format::errorCode(result.error().code);
            report.errorDetail = QString::fromStdString(result.error().detail);
            return report;
        }
        report.outcome = result->outcome;
        report.steps = std::move(result->steps);
        return report;
    });
    watcher_.setFuture(future);
}

void RunController::cancelRun() {
    if (running_) {
        engine_.cancel(engine::RunId{currentRunId_.load(std::memory_order_acquire)});
    }
}

void RunController::publishEvent(const ce::RunEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ce::RunStarted>) {
                currentRunId_.store(e.runId.value, std::memory_order_release);
                emit runStarted(QString::fromStdString(e.target.value),
                                static_cast<int>(e.chainSize),
                                QString::fromStdString(e.envName));
            } else if constexpr (std::is_same_v<T, ce::StepStarted>) {
                emit stepStarted(
                    static_cast<int>(e.stepIndex), QString::fromStdString(e.op.value), e.attempt);
            } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
                emit stepSkipped(static_cast<int>(e.stepIndex),
                                 QString::fromStdString(e.op.value),
                                 format::skipReason(e.reason));
            } else if constexpr (std::is_same_v<T, ce::RequestPrepared>) {
                emit requestPrepared(static_cast<int>(e.stepIndex),
                                     format::method(e.method),
                                     QString::fromStdString(e.url),
                                     joinHeaders(e.maskedHeaders),
                                     static_cast<int>(e.bodySize));
            } else if constexpr (std::is_same_v<T, ce::ResponseReceived>) {
                // Body is present only when the run opted in; empty optional
                // → empty QString, which the panel renders as "not captured".
                emit responseReceived(static_cast<int>(e.stepIndex),
                                      e.status,
                                      joinHeaders(e.headers),
                                      static_cast<int>(e.bodySize),
                                      static_cast<qint64>(e.elapsed.count()),
                                      e.body ? QString::fromStdString(*e.body) : QString{});
            } else if constexpr (std::is_same_v<T, ce::ExtractionCompleted>) {
                emit extractionCompleted(static_cast<int>(e.stepIndex),
                                         QString::fromStdString(e.op.value),
                                         QString::fromStdString(e.variableName),
                                         QString::fromStdString(e.sourcePath),
                                         format::extractionOutcome(e.outcome),
                                         QString::fromStdString(e.value));
            } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
                emit stepFailed(static_cast<int>(e.stepIndex),
                                QString::fromStdString(e.op.value),
                                format::errorCode(e.code),
                                QString::fromStdString(e.detail));
            } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
                emit runEnded(format::runOutcome(e.outcome));
            }
            // RunStarted/StepStarted/… cover the events the timeline renders;
            // ExtractionApplied, StepCancelled, SessionRefreshed are folded
            // into the finer-grained signals above or the final RunReport.
        },
        event);
}

}  // namespace chainapi::desktop
