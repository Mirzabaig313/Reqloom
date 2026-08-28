// Drives engine runs off the GUI thread and re-emits streamed RunEvents as
// Qt signals with native-typed payloads (QString/int/qint64) so the
// cross-thread connections to the views queue automatically.
//
// Threading: engine::ExecutionEngine::run blocks (real HTTP, polling sleeps),
// so it runs on a QtConcurrent worker. The engine's event callback fires on
// that worker thread but only emits signals — it never calls back into the
// engine — which is the marshalling discipline requires.
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
#include <cstdint>
#include <map>

namespace reqloom::desktop {

class ProjectModel;

/// One-shot request override for an Override-Mode run. When
/// `active`, the controller deep-copies the project, patches the target
/// operation with these values, and runs against the copy — the loaded project
/// is never mutated, so the override applies to this run only.
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

        // ponytail: Keep shapes a map cannot preserve in the raw path until
        // Operation supports ordered, multi-value query parameters.
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

/// Apply a RequestOverride's fields onto an operation in place. Shared by the
/// one-shot run path (patches a throwaway project copy) and the Save-to-Project
/// path (patches the real operation before writing YAML).
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

    /// Point the controller at a different project (workspace switch). Each
    /// project keeps its own RunContext (sessions + extraction cache), keyed by
    /// project root, so switching collections preserves each one's logins.
    /// Must not be called while a run is in flight (the caller blocks switches
    /// during a run).
    void setProject(const ProjectModel& project) noexcept;

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

    /// Pin concrete values for `{{resource.var}}` tokens (the value picker,
    /// Option A). Each pair is ("resource.var", value). Before each run these
    /// are seeded as the resource's most-recent instance, so the producing op
    /// is extraction-cache-skipped and the chain uses the chosen value.
    void setVariableOverrides(std::vector<std::pair<std::string, std::string>> overrides);

    /// Clear the run context's extraction cache only (sessions kept). Used when
    /// a pin changes so a stale pinned value can't linger into the next run.
    void clearExtractionCache();

    /// Snapshot of an actor's accumulated cookie jar (name → value) for the
    /// current run context, or empty when no run has populated a context yet.
    /// Read-only view for a cookie inspector.
    [[nodiscard]] std::map<std::string, std::string> cookies(const engine::ActorId& actor) const;

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
    void assertionCompleted(int index, QString op, QString name, QString expr, bool passed);
    void stepFailed(int index, QString op, QString code, QString detail, QVariantList diagnostics);
    void stepBlocked(int index, QString op, int blockedByIndex);
    void runEnded(QString outcome);

    /// Emitted on the GUI thread once the worker finishes. Carries the full
    /// chain summary for the timeline/response panels.
    void runFinished(RunReport report);

    /// Convenience signals for enabling/disabling controls.
    void runningChanged(bool running);

private:
    void publishEvent(const engine::RunEvent& event);

    /// The active project's RunContext, or nullptr when none has been created
    /// yet (no run since this project became active). Read paths use this.
    [[nodiscard]] engine::RunContext* findActiveContext() const;
    /// The active project's RunContext, creating it on first use. Run paths
    /// use this.
    engine::RunContext& contextForActive();

    engine::ExecutionEngine& engine_;
    const ProjectModel* project_;

    // Per-project run contexts keyed by project root, so switching collections
    // preserves each project's sessions + extraction cache. Node references are
    // stable (no eviction), so a worker capturing `&ctx` stays valid across a
    // switch (switches are blocked during a run anyway).
    // contexts are never evicted — session-lifetime memory. If a
    // workspace churns through many projects, evict on close instead.
    std::map<std::string, std::unique_ptr<engine::RunContext>> contexts_;
    QFutureWatcher<RunReport> watcher_;
    bool running_{false};
    bool captureResponseBodies_{false};
    // Pinned `{{resource.var}}` → value, seeded before each run (value picker).
    std::vector<std::pair<std::string, std::string>> variableOverrides_;
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

}  // namespace reqloom::desktop
