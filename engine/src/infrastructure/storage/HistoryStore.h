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

/// One row in the run-list view shown by the desktop history pane.
/// Denormalised from RunStarted / RunEnded events so the pane can
/// render a list without re-walking every event.
struct RunHistoryRow {
    RunId runId;
    OperationId targetOp;
    std::string envName;
    /// ISO-8601 UTC. Always set once RunStarted has been persisted.
    std::string startedAt;
    /// Empty until RunEnded is persisted.
    std::string endedAt;
    /// Empty until RunEnded is persisted ("Succeeded" / "Failed" / "Cancelled").
    std::string outcome;
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

    /// Open (and create if missing) the on-disk database. Schema is
    /// created or migrated as needed. The implementation MUST be safe
    /// to call once per process — repeated opens with the same path
    /// must be idempotent.
    virtual std::expected<void, ChainApiError> open(const std::filesystem::path& dbPath) = 0;

    /// Persist one observability event. Implementations MUST extract
    /// the run_id from the event variant and link the row to the
    /// matching `runs` entry, creating it on demand from the
    /// `RunStarted` event.
    virtual std::expected<void, ChainApiError> append(const RunEvent& event) = 0;

    /// Replay every event for one run, in seq order.
    [[nodiscard]] virtual std::expected<std::vector<RunEvent>, ChainApiError> eventsFor(
        RunId run) const = 0;

    /// List runs newest-first, capped at `limit` rows. The desktop
    /// history pane uses this to render the sidebar without paging
    /// through the event table. Pass 0 for "no limit".
    [[nodiscard]] virtual std::expected<std::vector<RunHistoryRow>, ChainApiError> listRuns(
        std::size_t limit = 100) const = 0;

    virtual void close() = 0;
};

}  // namespace chainapi::engine
