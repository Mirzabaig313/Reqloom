// RunController — see header. Off-thread engine execution + event marshalling.
#include "RunController.h"

#include "../views/Formatting.h"
#include "ProjectModel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <map>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace reqloom::desktop {

namespace {

namespace ce = reqloom::engine;

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
    : QObject(parent), engine_(engine), project_(&project) {
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

void RunController::setProject(const ProjectModel& project) noexcept {
    project_ = &project;
}

engine::RunContext* RunController::findActiveContext() const {
    const auto it = contexts_.find(project_->rootPath().toStdString());
    return it == contexts_.end() ? nullptr : it->second.get();
}

engine::RunContext& RunController::contextForActive() {
    auto& slot = contexts_[project_->rootPath().toStdString()];
    if (!slot) {
        slot = std::make_unique<engine::RunContext>();
    }
    return *slot;
}

void RunController::resetCaches() {
    if (running_) {
        return;
    }
    if (auto* ctx = findActiveContext(); ctx != nullptr) {
        ctx->clearExtractions();
        if (project_->hasProject()) {
            for (const auto& [actorId, _] : project_->project().actors) {
                ctx->invalidateSession(actorId);
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

void RunController::setVariableOverrides(
    std::vector<std::pair<std::string, std::string>> overrides) {
    variableOverrides_ = std::move(overrides);
}

void RunController::clearExtractionCache() {
    if (running_) {
        return;
    }
    if (auto* ctx = findActiveContext(); ctx != nullptr) {
        ctx->clearExtractions();
    }
}

std::map<std::string, std::string> RunController::cookies(const ce::ActorId& actor) const {
    const auto* ctx = findActiveContext();
    if (ctx == nullptr) {
        return {};
    }
    return ctx->cookies(actor);
}

RunController::ActorSessionInfo RunController::sessionInfo(const ce::ActorId& actor) const {
    const auto* ctx = findActiveContext();
    if (ctx == nullptr) {
        return {};
    }
    const ce::ActorSession* session = ctx->session(actor);
    if (session == nullptr) {
        return {};
    }

    ActorSessionInfo info;
    info.state = session->state;
    if (info.state == ce::ActorSession::State::Live) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            session->expiresAt - std::chrono::steady_clock::now());
        info.secondsRemaining =
            static_cast<int>(std::max<std::chrono::seconds::rep>(0, remaining.count()));
    }
    return info;
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
    applyOverrideToOperation(opIt->second, ov);
    return copy;
}

}  // namespace

void applyOverrideToOperation(ce::Operation& op, const RequestOverride& ov) {
    // Full-assignment, not partial-update: the editor always seeds every
    // control from the current operation, so the override snapshot is a
    // complete representation of the request. Assigning every field keeps the
    // one-shot-run path and the Save-to-Project path identical, and lets the
    // user actually clear a value (uncheck Force, pick no actor, etc.).
    op.method = methodFromLabel(ov.method);
    op.pathTemplate = ov.path.toStdString();
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

    // Actor: an empty selection clears it (the "(none)" combo entry).
    op.actor = ce::ActorId{ov.actor.toStdString()};

    // Inline auth: full-assignment — nullopt clears any prior per-op auth.
    op.inlineAuth = ov.inlineAuth;

    const auto codes = parseStatusList(ov.expectStatus);
    op.expectStatusList = codes;
    if (codes.empty()) {
        op.expectStatus.reset();
    } else {
        op.expectStatus = codes.front();
    }

    if (ov.timeoutMs > 0) {
        op.timeout = std::chrono::milliseconds{ov.timeoutMs};
    } else {
        op.timeout.reset();
    }
    op.force = ov.forceReRun;

    // Chain edits are opt-in: only touch depends_on / extract when the editor
    // actually loaded and re-snapshotted them, so a request-only override
    // leaves an operation's wiring intact.
    if (ov.chainEdited) {
        op.explicitDependencies.clear();
        op.explicitDependencies.reserve(ov.dependencies.size());
        for (const auto& dep : ov.dependencies) {
            op.explicitDependencies.push_back(ce::OperationId{dep});
        }
        op.extractions = ov.extractions;
        op.assertions = ov.assertions;
    }
}

void RunController::runWithOverride(const QString& target,
                                    const QString& environment,
                                    bool clean,
                                    bool dryRun,
                                    const RequestOverride& requestOverride) {
    if (running_ || !project_->hasProject()) {
        return;
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
        requestOverride.active ? patchedProject(project_->project(), targetId, requestOverride)
                               : project_->projectPtr();
    ce::RunContext& ctx = contextForActive();

    // Value-picker pins (Option A): seed each chosen value as the resource's
    // most-recent instance so the producing op is extraction-cache-skipped and
    // the chain uses the pinned value. A clean run resets extractions inside
    // the engine, so pins are intentionally ignored there (truly fresh).
    for (const auto& [token, value] : variableOverrides_) {
        const auto dot = token.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        ce::ResourceInstance instance;
        instance.variables[token.substr(dot + 1)] = value;
        ctx.appendInstance(ce::ResourceId{token.substr(0, dot)}, std::move(instance));
    }

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
                emit stepStarted(format::boundedIndex(e.stepIndex),
                                 QString::fromStdString(e.op.value),
                                 e.attempt);
            } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
                emit stepSkipped(format::boundedIndex(e.stepIndex),
                                 QString::fromStdString(e.op.value),
                                 format::skipReason(e.reason));
            } else if constexpr (std::is_same_v<T, ce::RequestPrepared>) {
                emit requestPrepared(format::boundedIndex(e.stepIndex),
                                     format::method(e.method),
                                     QString::fromStdString(e.url),
                                     joinHeaders(e.maskedHeaders),
                                     static_cast<int>(e.bodySize));
            } else if constexpr (std::is_same_v<T, ce::ResponseReceived>) {
                // Body is present only when the run opted in; empty optional
                // → empty QString, which the panel renders as "not captured".
                emit responseReceived(format::boundedIndex(e.stepIndex),
                                      e.status,
                                      joinHeaders(e.headers),
                                      static_cast<int>(e.bodySize),
                                      static_cast<qint64>(e.elapsed.count()),
                                      e.body ? QString::fromStdString(*e.body) : QString{});
            } else if constexpr (std::is_same_v<T, ce::ExtractionCompleted>) {
                emit extractionCompleted(format::boundedIndex(e.stepIndex),
                                         QString::fromStdString(e.op.value),
                                         QString::fromStdString(e.variableName),
                                         QString::fromStdString(e.sourcePath),
                                         format::extractionOutcome(e.outcome),
                                         QString::fromStdString(e.value));
            } else if constexpr (std::is_same_v<T, ce::AssertionCompleted>) {
                emit assertionCompleted(format::boundedIndex(e.stepIndex),
                                        QString::fromStdString(e.op.value),
                                        QString::fromStdString(e.name),
                                        QString::fromStdString(e.expr),
                                        e.passed);
            } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
                emit stepFailed(format::boundedIndex(e.stepIndex),
                                QString::fromStdString(e.op.value),
                                format::errorCode(e.code),
                                QString::fromStdString(e.detail),
                                format::unresolvedDiagnostics(e.diagnostics));
            } else if constexpr (std::is_same_v<T, ce::StepBlocked>) {
                emit stepBlocked(format::boundedIndex(e.stepIndex),
                                 QString::fromStdString(e.op.value),
                                 format::boundedIndex(e.blockedByStepIndex));
            } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
                emit runEnded(format::runOutcome(e.outcome));
            }
            // The timeline renders every event with user-facing evidence;
            // ExtractionApplied, StepCancelled, and SessionRefreshed remain
            // folded into finer-grained events or the final RunReport.
        },
        event);
}

}  // namespace reqloom::desktop
