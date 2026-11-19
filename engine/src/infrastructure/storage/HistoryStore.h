// Engine-internal interface for persisting run logs.
// Concrete impl: SqliteHistoryStore.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Events.h>
#include <reqloom/engine/RunContext.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::engine {

/// One row in the desktop history sidebar, denormalised from
/// RunStarted / RunEnded so the pane needn't replay every event.
struct RunHistoryRow {
    RunId runId;
    OperationId targetOp;
    std::string envName;
    std::string startedAt;  ///< ISO-8601 UTC, set once RunStarted lands.
    std::string endedAt;    ///< Empty until RunEnded.
    std::string outcome;    ///< Empty, or Succeeded / Failed / Cancelled.
    std::size_t chainSize{0};
    std::int64_t elapsedMs{-1};  ///< Wall-clock run duration; -1 until RunEnded lands.
    RunInitiator initiator{};    ///< From RunStarted; `App` for rows written before FR-15.8.
    bool hasRecordedSteps{false};
};

class HistoryStore {
public:
    HistoryStore() = default;
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;
    HistoryStore(HistoryStore&&) = delete;
    HistoryStore& operator=(HistoryStore&&) = delete;
    virtual ~HistoryStore() = default;

    /// Open (creating if missing) the database and its schema.
    /// Idempotent across repeated opens of the same path.
    virtual std::expected<void, ReqloomError> open(const std::filesystem::path& dbPath) = 0;

    /// Open an existing, up-to-date database for reading only: nothing is created or upgraded,
    /// and every write fails. Errors when the file is missing or predates this build's schema.
    virtual std::expected<void, ReqloomError> openReadOnly(const std::filesystem::path& dbPath) {
        (void)dbPath;
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "history: read-only open unsupported"});
    }

    /// Reserve a run id never used by any run in this database, across processes and restarts.
    /// Errors when the store is not open; the engine then falls back to a process-local id.
    [[nodiscard]] virtual std::expected<RunId, ReqloomError> allocateRunId() {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "history: run id allocation unsupported"});
    }

    /// Persist one event, linking it to its run (created on demand from
    /// the RunStarted event). A RunStarted whose id already belongs to a started run is rejected,
    /// along with every later event for it, so runs never merge.
    virtual std::expected<void, ReqloomError> append(const RunEvent& event) = 0;

    /// Replay every event for one run, in seq order.
    [[nodiscard]] virtual std::expected<std::vector<RunEvent>, ReqloomError> eventsFor(
        RunId run) const = 0;

    /// Store a run's masked step rows (agent-facing evidence, `ExecutionEngine::recordedSteps`),
    /// replacing any stored before for that run. The run row must exist (its RunStarted landed).
    virtual std::expected<void, ReqloomError> appendSteps(RunId run,
                                                          const std::vector<StepResult>& steps) {
        (void)run;
        (void)steps;
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "history: step records unsupported"});
    }

    /// Rewrite every stored event of `run` through `edit` (masking them again once the run knows
    /// all its secrets). Only for a run this store started; others are refused.
    virtual std::expected<void, ReqloomError> rewriteEvents(
        RunId run, const std::function<void(RunEvent&)>& edit) {
        (void)run;
        (void)edit;
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "history: event rewrite unsupported"});
    }

    /// The step rows `appendSteps` stored for `run`, in order; empty when none were.
    [[nodiscard]] virtual std::expected<std::vector<StepResult>, ReqloomError> stepsFor(
        RunId run) const {
        (void)run;
        return std::vector<StepResult>{};
    }

    /// Runs newest-first, capped at `limit` (0 = no limit).
    [[nodiscard]] virtual std::expected<std::vector<RunHistoryRow>, ReqloomError> listRuns(
        std::size_t limit = 100) const = 0;

    /// Delete all runs and their events from the open database.
    virtual std::expected<void, ReqloomError> clear() = 0;

    virtual void close() = 0;
};

}  // namespace reqloom::engine
