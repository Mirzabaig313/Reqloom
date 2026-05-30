// RunController — see header. Off-thread engine execution + event marshalling.
#include "RunController.h"

#include "../views/Formatting.h"
#include "ProjectModel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <map>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
    // public API) and outlives this controller (App member order). The
    // callback captures a copy of the shared `alive` flag — not a bare `this`
    // — and bails once `~RunController` clears it, so an event delivered after
    // destruction can never dereference a freed `this`.
    engine_.subscribe([this, alive = alive_](const ce::RunEvent& event) {
        if (alive->load(std::memory_order_acquire)) {
            publishEvent(event);
        }
    });

    connect(&watcher_, &QFutureWatcher<RunReport>::finished, this, [this]() {
        const RunReport report = watcher_.result();
        running_ = false;
        emit runningChanged(false);
        emit runFinished(report);
    });
}

RunController::~RunController() {
    // Block any further callback work, then drain the worker so an in-flight
    // run() that is mid-emit can't race the teardown of this object.
    alive_->store(false, std::memory_order_release);
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
    runWithOverride(target, environment, clean, dryRun, RequestOverride{});
}

namespace {

/// Map a method label to the engine enum. Unknown → GET (the safe default).
[[nodiscard]] ce::HttpMethod methodFromLabel(const QString& label) {
    const QString m = label.trimmed().toUpper();
    if (m == QStringLiteral("POST")) {
        return ce::HttpMethod::Post;
    }
    if (m == QStringLiteral("PUT")) {
        return ce::HttpMethod::Put;
    }
    if (m == QStringLiteral("PATCH")) {
        return ce::HttpMethod::Patch;
    }
    if (m == QStringLiteral("DELETE")) {
        return ce::HttpMethod::Delete;
    }
    if (m == QStringLiteral("HEAD")) {
        return ce::HttpMethod::Head;
    }
    if (m == QStringLiteral("OPTIONS")) {
        return ce::HttpMethod::Options;
    }
    return ce::HttpMethod::Get;
}

/// Parse a comma-separated status list ("200,201") into the engine's vector.
/// Non-numeric tokens are skipped.
[[nodiscard]] std::vector<int> parseStatusList(const QString& text) {
    std::vector<int> out;
    const auto tokens = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        bool ok = false;
        const int code = token.trimmed().toInt(&ok);
        if (ok) {
            out.push_back(code);
        }
    }
    return out;
}

/// Apply a one-shot override to a copy of `base`, patching the target
/// operation's request fields. Returns a fresh Project; `base` is untouched.
[[nodiscard]] std::shared_ptr<const ce::Project> patchedProject(const ce::Project& base,
                                                                const ce::OperationId& target,
                                                                const RequestOverride& ov) {
    auto copy = std::make_shared<ce::Project>(base);

    const auto dot = target.value.find('.');
    if (dot == std::string::npos) {
        return copy;
    }
    const ce::ResourceId resId{target.value.substr(0, dot)};
    const auto opName = target.value.substr(dot + 1);

    auto resIt = copy->resources.find(resId);
    if (resIt == copy->resources.end()) {
        return copy;
    }
    auto opIt = resIt->second.operations.find(opName);
    if (opIt == resIt->second.operations.end()) {
        return copy;
    }
    ce::Operation& op = opIt->second;

    if (!ov.method.isEmpty()) {
        op.method = methodFromLabel(ov.method);
    }
    if (!ov.path.isEmpty()) {
        op.pathTemplate = ov.path.toStdString();
    }
    // Headers / query params: the editor shows the full set, so a patch
    // replaces them wholesale (what you see is what gets sent).
    op.headers = ov.headers;
    op.queryParams = ov.queryParams;

    // Body: form vs raw are mutually exclusive shapes.
    if (ov.bodyIsForm) {
        op.bodyForm = ov.formFields;
        op.bodyTemplate.reset();
    } else {
        op.bodyForm.reset();
        const QString trimmed = ov.body.trimmed();
        if (trimmed.isEmpty() || trimmed == QStringLiteral("(no body)")) {
            op.bodyTemplate.reset();
        } else {
            op.bodyTemplate = trimmed.toStdString();
        }
    }

    if (!ov.actor.isEmpty()) {
        op.actor = ce::ActorId{ov.actor.toStdString()};
    }

    const auto codes = parseStatusList(ov.expectStatus);
    if (!codes.empty()) {
        op.expectStatusList = codes;
        op.expectStatus = codes.front();
    }

    if (ov.timeoutMs > 0) {
        op.timeout = std::chrono::milliseconds{ov.timeoutMs};
    }
    if (ov.forceReRun) {
        op.force = true;
    }
    return copy;
}

}  // namespace

void RunController::runWithOverride(const QString& target,
                                    const QString& environment,
                                    bool clean,
                                    bool dryRun,
                                    const RequestOverride& requestOverride) {
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
    // Strong handle to the project the worker runs against. For an override
    // run this is a patched deep copy (one-shot); otherwise it's the shared
    // loaded project. Either way the worker owns a strong ref, so a concurrent
    // reload can't dangle it.
    const std::shared_ptr<const ce::Project> project =
        requestOverride.active ? patchedProject(project_.project(), targetId, requestOverride)
                               : project_.projectPtr();
    ce::RunContext& ctx = *context_;

    running_ = true;
    emit runningChanged(true);

    // Clear any prior run's id before the new run's RunStarted lands, so a
    // cancelRun() racing the worker startup can't cancel by a stale id.
    currentRunId_.store(0, std::memory_order_release);

    // engine_.run blocks; run it on a worker. The captured state outlives the
    // run: the engine lives for the app lifetime, `project` is a shared handle
    // owned by the lambda, and ctx is a member owned by this controller (whose
    // destructor drains the worker before teardown).
    auto future = QtConcurrent::run([this, project, targetId, &ctx, options]() -> RunReport {
        RunReport report;
        auto result = engine_.run(*project, targetId, ctx, options);
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
    if (!running_) {
        return;
    }
    // A run is in flight but its RunStarted may not have arrived yet (id still
    // 0). Cancelling by a zero id is a no-op in the engine, so only forward a
    // real id; the worker also checks cancellation each step once it starts.
    const auto runId = currentRunId_.load(std::memory_order_acquire);
    if (runId != 0) {
        engine_.cancel(engine::RunId{runId});
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
