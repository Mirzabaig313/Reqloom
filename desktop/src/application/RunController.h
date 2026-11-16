// Drives engine runs off the GUI thread and re-emits streamed RunEvents as Qt signals with
// native-typed payloads, so cross-thread connections queue. Threading: ExecutionEngine::run blocks,
// so it runs on a QtConcurrent worker; the event callback fires on that worker and only emits
// signals, never calling back into the engine.
#pragma once

#include "application/UrlTemplate.h"

#include <reqloom/engine/PublicApi.h>

#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVariantList>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>

namespace reqloom::desktop {

class ProjectModel;

/// One-shot request override for an Override-Mode run: when `active`, the controller patches a deep
/// copy of the project, so the loaded project is never mutated.
struct RequestOverride {
    bool active{false};

    QString method;                                  ///< e.g. "POST" (empty → unchanged)
    QString path;                                    ///< path template (empty → unchanged)
    std::map<std::string, std::string> headers;      ///< replaces op headers
    std::map<std::string, std::string> queryParams;  ///< replaces op query params

    /// Move a map-representable query suffix from `path` into `queryParams`.
    /// Embedded items win existing keys; lossy query shapes stay in `path`.
    void normalizePathQuery() {
        const qsizetype queryStart = url_template::findDelimiter(path, QLatin1Char('?'));
        const qsizetype fragmentStart = url_template::findDelimiter(path, QLatin1Char('#'));
        if (queryStart < 0 || (fragmentStart >= 0 && fragmentStart < queryStart)) {
            return;
        }

        const qsizetype queryLength = fragmentStart < 0 ? -1 : fragmentStart - queryStart - 1;
        const QString rawQuery = path.mid(queryStart + 1, queryLength);
        const auto rawItems = url_template::splitOutsideTemplates(rawQuery, QLatin1Char('&'));
        for (const QString& rawItem : rawItems) {
            const qsizetype equalsIndex{url_template::findDelimiter(rawItem, QLatin1Char('='))};
            const QString rawKey{equalsIndex >= 0 ? rawItem.left(equalsIndex) : rawItem};
            if (rawKey.isEmpty()) {
                continue;
            }
            const QString key{QUrl::fromPercentEncoding(rawKey.toUtf8())};
            if (!key.contains(QStringLiteral("{{"))) {
                queryParams.erase(key.toStdString());
            }
        }
        if (rawQuery.contains(QLatin1Char('%')) || rawQuery.contains(QLatin1Char('{')) ||
            rawQuery.contains(QLatin1Char('}')) || rawQuery.contains(QLatin1Char('+'))) {
            return;
        }

        std::vector<std::pair<QString, QString>> items{};
        items.reserve(static_cast<std::size_t>(rawItems.size()));
        for (const QString& rawItem : rawItems) {
            const qsizetype equalsIndex{url_template::findDelimiter(rawItem, QLatin1Char('='))};
            if (equalsIndex <= 0) {
                return;
            }
            const QString key{QUrl::fromPercentEncoding(rawItem.left(equalsIndex).toUtf8())};
            const QString value{
                QUrl::fromPercentEncoding(rawItem.sliced(equalsIndex + 1).toUtf8())};
            if (key.contains(QStringLiteral("{{")) ||
                url_template::containsExplicitUrlEncode(value)) {
                return;
            }
            items.emplace_back(key, value);
        }

        // Keep shapes a map cannot preserve in the raw path until Operation supports ordered,
        // multi-value query parameters.
        std::string previousKey;
        bool first{true};
        for (const auto& item : items) {
            const std::string decodedKey = item.first.toStdString();
            if (!first && decodedKey <= previousKey) {
                return;
            }
            previousKey = decodedKey;
            first = false;
        }

        const QString fragment{fragmentStart >= 0 ? path.sliced(fragmentStart) : QString{}};
        path = path.left(queryStart) + fragment;
        for (const auto& [key, value] : items) {
            queryParams.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }

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

    /// Chain wiring. Only applied when `chainEdited` is set, so a request-only
    /// override doesn't wipe an operation's depends_on / extract.
    bool chainEdited{false};
    std::vector<std::string> dependencies;        ///< replaces explicitDependencies
    std::vector<engine::Extraction> extractions;  ///< replaces extractions
    std::vector<engine::Assertion> assertions;    ///< replaces assertions

    /// Per-operation inline (actor-less) auth. nullopt clears any existing
    /// auth on the operation (full-assignment semantics, like headers).
    std::optional<engine::InlineAuth> inlineAuth;
};

/// Apply a RequestOverride onto an operation in place; shared by the one-shot run path and
/// Save-to-Project.
void applyOverrideToOperation(engine::Operation& op, const RequestOverride& ov);

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

    /// Point the controller at a different project (workspace switch). Must not be called while a
    /// run is in flight.
    void setProject(const ProjectModel& project) noexcept;

    /// Resets the active environment's run context (sessions, cookie jar, extraction).
    /// Refused while a run is in flight.
    void resetCaches();

    /// Capture full response bodies for the next run (off by default: the engine's redaction-first
    /// contract keeps bodies out of events and history). Login/refresh bodies, tokens included,
    /// become visible too, so surface it as a deliberate user choice.
    void setCaptureResponseBodies(bool capture) noexcept;
    [[nodiscard]] bool captureResponseBodies() const noexcept;

    /// Pin concrete values for `{{resource.var}}` tokens as ("resource.var", value) pairs, seeded
    /// before each run so the producing op is skipped and the chosen value used.
    void setVariableOverrides(std::vector<std::pair<std::string, std::string>> overrides);

    /// Clear the extraction cache only (sessions kept), so a changed pin can't leave a stale value.
    void clearExtractionCache();

    /// An actor's accumulated cookie jar (name → value) for the current run context; empty before
    /// any run. While a run uses that context, this and the other read paths below answer from the
    /// copy taken just before the run started, so the GUI thread never reads what the worker
    /// writes.
    [[nodiscard]] std::map<std::string, std::string> cookies(const engine::ActorId& actor) const;

    /// The same jar with each cookie's setting host, Domain, Path, Secure and expiry.
    [[nodiscard]] std::vector<engine::JarCookie> jarCookies(const engine::ActorId& actor) const;

    /// Read-only view of one actor's auth session for the explorer's status indicator; reuses the
    /// engine's state enum.
    struct ActorSessionInfo {
        engine::ActorSession::State state{engine::ActorSession::State::None};
        /// Whole seconds until the session lapses; 0 unless `state` is `Live` with time left.
        int secondsRemaining{0};
    };

    /// The actor's session in the active run context; a default value (state `None`) means not
    /// authenticated, reset, or no context.
    [[nodiscard]] ActorSessionInfo sessionInfo(const engine::ActorId& actor) const;

    /// Select the environment whose run context the read and reset paths use; empty means
    /// the project's default. A run also selects its environment.
    void setEnvironment(const QString& environment);

    /// Drop every run context of the project at `rootPath`. Refused for an in-flight run's project.
    void forgetProject(const QString& rootPath);

    /// Drop the run context of a deleted `environment` in the project at `rootPath`, so a later
    /// environment of the same name starts clean. An in-flight run's context goes when the run
    /// ends.
    void forgetEnvironment(const QString& rootPath, const QString& environment);

    /// Keep a renamed environment's run context (sessions, cookies, extracted values) under its
    /// new name, replacing anything kept under that name. An in-flight run's context moves when
    /// the run ends.
    void renameEnvironment(const QString& rootPath, const QString& from, const QString& to);

    /// The active run context, or nullptr when none exists yet. Not for the GUI thread while a run
    /// uses that context.
    [[nodiscard]] engine::RunContext* findActiveContext() const;
    /// The active run context, created on first use.
    engine::RunContext& contextForActive();

public slots:
    /// Kick off a run ending at `target` against `environment` (empty → project
    /// default). `dryRun` previews the resolved chain without sending requests.
    /// `clean` invalidates sessions + extractions before running. No-op if a
    /// run is already in flight.
    void run(const QString& target, const QString& environment, bool clean, bool dryRun);

    /// Run with a one-shot override applied; behaves exactly like `run` when `override.active` is
    /// false.
    void runWithOverride(const QString& target,
                         const QString& environment,
                         bool clean,
                         bool dryRun,
                         const RequestOverride& requestOverride);

    /// Cancel the in-flight run, if any. A cancel issued before the engine has announced the run
    /// id is held and applied as soon as it does.
    void cancelRun();

signals:
    void runStarted(QString target, int chainSize, QString environment);
    void stepStarted(int index, QString op, int attempt);
    void stepSkipped(int index, QString op, QString reason);
    /// `exchange` is the engine's `exchangeKindName` ("operation", "sign_in", …).
    void requestPrepared(int index,
                         QString method,
                         QString url,
                         QString maskedHeaders,
                         int bodySize,
                         QString exchange);
    void responseReceived(int index,
                          int status,
                          QString headers,
                          int bodySize,
                          qint64 elapsedMs,
                          QString body,
                          QString exchange);
    void extractionCompleted(int index,
                             QString op,
                             QString variableName,
                             QString sourcePath,
                             QString outcome,
                             QString value);
    /// `detail` says why a failed assertion failed; empty when it passed.
    void assertionCompleted(
        int index, QString op, QString name, QString expr, bool passed, QString detail);
    void stepFailed(int index, QString op, QString code, QString detail, QVariantList diagnostics);
    void stepBlocked(int index, QString op, int blockedByIndex);
    void runEnded(QString outcome);

    /// A run was not started; `message` says why (for example, its environment no longer exists).
    void runRefused(QString message);

    /// Emitted on the GUI thread once the worker finishes, carrying the chain summary.
    void runFinished(RunReport report);

    /// Convenience signals for enabling/disabling controls.
    void runningChanged(bool running);

private:
    // Shared with the engine callback, which the engine keeps after this controller is gone.
    struct EventGate {
        std::atomic_bool alive{true};
        std::atomic<std::uint64_t> runId{0};
        std::atomic_bool cancelPending{false};
    };

    // What the GUI may read about one actor while a run owns its context.
    struct ActorView {
        engine::ActorSession::State state{engine::ActorSession::State::None};
        std::chrono::steady_clock::time_point expiresAt{};
        std::map<std::string, std::string> cookies;
        std::vector<engine::JarCookie> jar;
    };

    void publishEvent(const engine::RunEvent& event);
    void requestCancel();
    void snapshotForRun(const engine::RunContext& ctx);
    // The pre-run copy of `actor` when a run is using the active context, else nullptr.
    [[nodiscard]] const ActorView* runSnapshotFor(const engine::ActorId& actor) const;
    [[nodiscard]] static ActorSessionInfo sessionInfoOf(
        engine::ActorSession::State state, std::chrono::steady_clock::time_point expiresAt);

    engine::ExecutionEngine& engine_;
    const ProjectModel* project_;

    // (project root, environment name) — the key of one run context.
    using ContextKey = std::pair<std::string, std::string>;
    [[nodiscard]] ContextKey activeKey() const;

    // One run context per (project, environment). Node references are stable, so a worker
    // holding `&ctx` stays valid across a switch and a rename. Evicted only by forgetProject and
    // forgetEnvironment.
    std::map<ContextKey, std::unique_ptr<engine::RunContext>> contexts_;
    std::string environment_;
    // Context of the in-flight run: forgetProject refuses its project, reads of it use
    // runSnapshot_.
    ContextKey runningKey_;
    // Set when the in-flight run's environment was deleted; its context goes at run end.
    bool dropRunningContext_{false};
    // Set when the in-flight run's environment was renamed; its context moves at run end.
    std::optional<std::string> renameRunningTo_;
    void moveContext(const ContextKey& from, const ContextKey& to);
    // Where the in-flight run's context will live once the run ends: its key, or the name it was
    // renamed to during the run.
    [[nodiscard]] ContextKey pendingRunningKey() const;
    std::map<std::string, ActorView> runSnapshot_;
    QFutureWatcher<RunReport> watcher_;
    bool running_{false};
    bool captureResponseBodies_{false};
    // Pinned `{{resource.var}}` → value, seeded before each run (value picker).
    std::vector<std::pair<std::string, std::string>> variableOverrides_;
    std::shared_ptr<EventGate> gate_{std::make_shared<EventGate>()};
};

}  // namespace reqloom::desktop
