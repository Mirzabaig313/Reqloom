// Drives engine runs off the GUI thread and re-emits streamed RunEvents as
// Qt signals with native-typed payloads (QString/int/qint64) so the
// cross-thread connections to the views queue automatically.
//
// Threading: engine::ExecutionEngine::run blocks (real HTTP, polling sleeps),
// so it runs on a QtConcurrent worker. The engine's event callback fires on
// that worker thread but only emits signals — it never calls back into the
// engine — which is the marshalling discipline AGENTS.md requires.
#pragma once

#include <chainapi/engine/PublicApi.h>

#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <vector>

#include <atomic>
#include <cstdint>

namespace chainapi::desktop {

class ProjectModel;

/// One-shot request override for an Override-Mode run (DESIGN.md §6.3). When
/// `active`, the controller deep-copies the project, patches the target
/// operation with these values, and runs against the copy — the loaded project
/// is never mutated, so the override applies to this run only.
struct RequestOverride {
    bool active{false};

    QString method;                                  ///< e.g. "POST" (empty → unchanged)
    QString path;                                    ///< path template (empty → unchanged)
    std::map<std::string, std::string> headers;      ///< replaces op headers
    std::map<std::string, std::string> queryParams;  ///< replaces op query params

    /// Body. When `bodyIsForm` is false, `body` is a raw template (empty →
    /// no body). When true, `formFields` is sent as form-data/multipart and
    /// `body` is ignored.
    bool bodyIsForm{false};
    QString body;
    std::map<std::string, std::string> formFields;

    QString actor;           ///< actor id to run as (empty → unchanged)
    QString expectStatus;    ///< comma-separated codes, e.g. "200,201" (empty → unchanged)
    int timeoutMs{0};        ///< per-op timeout in ms (0 → unchanged)
    bool forceReRun{false};  ///< ignore the extraction cache for this op
};

/// Outcome of one run, handed from the worker back to the GUI thread.
struct RunReport {
    bool engineError{false};  ///< true → schema-time failure, see errorCode.
    QString errorCode;
    QString errorDetail;
    engine::RunOutcome outcome{engine::RunOutcome::Succeeded};
    std::vector<engine::StepResult> steps;
};

class RunController : public QObject {
    Q_OBJECT

public:
    RunController(engine::ExecutionEngine& engine, const ProjectModel& project, QObject* parent);
    ~RunController() override;

    RunController(const RunController&) = delete;
    RunController& operator=(const RunController&) = delete;
    RunController(RunController&&) = delete;
    RunController& operator=(RunController&&) = delete;

    [[nodiscard]] bool isRunning() const noexcept;

    /// Resets the run context's session + extraction caches. Refused while
    /// a run is in flight.
    void resetCaches();

    /// Opt into capturing full response bodies for the next run. Off by
    /// default — the engine's redaction-first contract keeps bodies off the
    /// event surface (and out of the history DB) unless the user asks. When
    /// on, login/refresh bodies (tokens included) become visible too, so the
    /// caller surfaces this as a deliberate user choice.
    void setCaptureResponseBodies(bool capture) noexcept;
    [[nodiscard]] bool captureResponseBodies() const noexcept;

public slots:
    /// Kick off a run ending at `target` against `environment` (empty → project
    /// default). `dryRun` previews the resolved chain without sending requests.
    /// `clean` invalidates sessions + extractions before running. No-op if a
    /// run is already in flight.
    void run(const QString& target, const QString& environment, bool clean, bool dryRun);

    /// Run with a one-shot override applied to the target operation (§6.3).
    /// When `override.active` is false this behaves exactly like `run`.
    void runWithOverride(const QString& target,
                         const QString& environment,
                         bool clean,
                         bool dryRun,
                         const RequestOverride& requestOverride);

    /// Cancel the in-flight run, if any.
    void cancelRun();

signals:
    void runStarted(QString target, int chainSize, QString environment);
    void stepStarted(int index, QString op, int attempt);
    void stepSkipped(int index, QString op, QString reason);
    void requestPrepared(
        int index, QString method, QString url, QString maskedHeaders, int bodySize);
    void responseReceived(
        int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString body);
    void extractionCompleted(int index,
                             QString op,
                             QString variableName,
                             QString sourcePath,
                             QString outcome,
                             QString value);
    void stepFailed(int index, QString op, QString code, QString detail);
    void runEnded(QString outcome);

    /// Emitted on the GUI thread once the worker finishes. Carries the full
    /// chain summary for the timeline/response panels.
    void runFinished(RunReport report);

    /// Convenience signals for enabling/disabling controls.
    void runningChanged(bool running);

private:
    void publishEvent(const engine::RunEvent& event);

    engine::ExecutionEngine& engine_;
    const ProjectModel& project_;

    std::unique_ptr<engine::RunContext> context_;
    QFutureWatcher<RunReport> watcher_;
    bool running_{false};
    bool captureResponseBodies_{false};
    // Written on the worker thread (RunStarted handler), read on the GUI
    // thread (cancelRun) — atomic to avoid a data race on the run id.
    std::atomic<std::uint64_t> currentRunId_{0};

    // Lifetime guard for the engine event callback. The engine retains the
    // callback for its whole life with no unsubscribe, and (per App's member
    // order) outlives this controller. The callback captures a copy of this
    // shared flag and skips emitting once the destructor clears it, so an
    // event fired after `~RunController` can never touch a freed `this`.
    std::shared_ptr<std::atomic_bool> alive_{std::make_shared<std::atomic_bool>(true)};
};

}  // namespace chainapi::desktop
