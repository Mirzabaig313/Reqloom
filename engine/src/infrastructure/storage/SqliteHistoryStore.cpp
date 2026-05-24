// SqliteHistoryStore — embedded SQLite-backed run-history persistence.
// FR-12 / Engine Req §10. Phase 2 implementation.
#include "SqliteHistoryStore.h"

namespace chainapi::engine {

SqliteHistoryStore::SqliteHistoryStore() = default;
SqliteHistoryStore::~SqliteHistoryStore() = default;

std::expected<void, ChainApiError>
SqliteHistoryStore::open(const std::filesystem::path& /*dbPath*/) {
    // Phase 2: open + create schema if missing.
    return {};
}

std::expected<void, ChainApiError>
SqliteHistoryStore::append(const RunEvent& /*event*/) {
    // Phase 2: serialise to the runs/events tables.
    return {};
}

std::expected<std::vector<RunEvent>, ChainApiError>
SqliteHistoryStore::eventsFor(RunId /*run*/) const {
    return std::vector<RunEvent>{};
}

void SqliteHistoryStore::close() {
    // Phase 2.
}

}  // namespace chainapi::engine
