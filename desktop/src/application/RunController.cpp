// RunController — see header. Off-thread engine execution + event marshalling.
#include "RunController.h"

#include "../views/Formatting.h"
#include "ProjectModel.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace reqloom::desktop {

namespace {

namespace ce = reqloom::engine;

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

    // The engine retains this callback with no unsubscribe and outlives this controller. Capture a
    // copy of the `alive` flag, not a bare `this`, so an event after destruction can't touch a
    // freed `this`.
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
    // Block further callback work, then drain the worker so an in-flight run() can't race teardown.
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

RunController::ContextKey RunController::activeKey() const {
    std::string env = environment_;
    if (env.empty() && project_->hasProject()) {
        env = project_->project().defaultEnvironment;
    }
    return {project_->rootPath().toStdString(), std::move(env)};
}

engine::RunContext* RunController::findActiveContext() const {
    const auto it = contexts_.find(activeKey());
    return it == contexts_.end() ? nullptr : it->second.get();
}

engine::RunContext& RunController::contextForActive() {
    auto& slot = contexts_[activeKey()];
    if (!slot) {
        slot = std::make_unique<engine::RunContext>();
    }
    return *slot;
}

void RunController::setEnvironment(const QString& environment) {
    environment_ = environment.toStdString();
}

void RunController::forgetProject(const QString& rootPath) {
    const std::string root = rootPath.toStdString();
    if (running_ && root == runningRoot_) {
        return;
    }
    std::erase_if(contexts_, [&root](const auto& entry) { return entry.first.first == root; });
}

void RunController::resetCaches() {
    if (running_) {
        return;
    }
    if (auto* ctx = findActiveContext(); ctx != nullptr) {
        ctx->clearExtractions();
        ctx->clearSessions();
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

std::vector<ce::JarCookie> RunController::jarCookies(const ce::ActorId& actor) const {
    const auto* ctx = findActiveContext();
    if (ctx == nullptr) {
        return {};
    }
    return ctx->jarCookies(actor);
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

/// Parse a comma-separated status list ("200,201"); non-numeric tokens are skipped.
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
    // Full assignment, not partial update: the editor seeds every control, so the override is a
    // complete request. This keeps the one-shot run and Save-to-Project paths identical and lets
    // the user clear a value.
    op.method = methodFromLabel(ov.method);
    op.pathTemplate = ov.path.toStdString();
    op.headers = ov.headers;
    op.queryParams = ov.queryParams;

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

    op.actor = ce::ActorId{ov.actor.toStdString()};

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

    // Chain edits are opt-in: a request-only override leaves the operation's wiring intact.
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
    // Strong handle to the worker's project (a patched copy for an override run), so a concurrent
    // reload can't dangle it.
    const std::shared_ptr<const ce::Project> project =
        requestOverride.active ? patchedProject(project_->project(), targetId, requestOverride)
                               : project_->projectPtr();
    setEnvironment(environment);
    ce::RunContext& ctx = contextForActive();
    runningRoot_ = project_->rootPath().toStdString();

    // Value-picker pins seed each chosen value as the resource's most-recent instance, so the
    // producing op is skipped and the pinned value used. A clean run resets extractions in the
    // engine, so pins are ignored there.
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

    // Clear the prior run id so a cancelRun() racing worker startup can't cancel by a stale id.
    currentRunId_.store(0, std::memory_order_release);

    // engine_.run blocks, so run it on a worker. Captured state outlives the run: the engine lives
    // for the app, `project` is owned by the lambda, and ctx is a member whose destructor drains
    // the worker.
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
    // RunStarted may not have arrived yet (id 0); cancelling by a zero id is a no-op, so forward
    // only a real id.
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
                // Body is empty unless the run opted in; the panel renders that as "not captured".
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
            // ExtractionApplied, StepCancelled and SessionRefreshed are folded into finer events or
            // the final RunReport.
        },
        event);
}

}  // namespace reqloom::desktop
