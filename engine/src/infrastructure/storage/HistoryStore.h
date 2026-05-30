// Engine-internal interface for persisting run logs.
// Concrete impl: SqliteHistoryStore.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

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
    virtual std::expected<void, ChainApiError> open(const std::filesystem::path& dbPath) = 0;

    /// Persist one event, linking it to its run (created on demand from
    /// the RunStarted event).
    virtual std::expected<void, ChainApiError> append(const RunEvent& event) = 0;

    /// Replay every event for one run, in seq order.
    [[nodiscard]] virtual std::expected<std::vector<RunEvent>, ChainApiError> eventsFor(
        RunId run) const = 0;

    /// Runs newest-first, capped at `limit` (0 = no limit).
    [[nodiscard]] virtual std::expected<std::vector<RunHistoryRow>, ChainApiError> listRuns(
        std::size_t limit = 100) const = 0;

    virtual void close() = 0;
};

}  // namespace chainapi::engine
