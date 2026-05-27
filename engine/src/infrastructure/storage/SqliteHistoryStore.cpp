// SqliteHistoryStore — embedded SQLite-backed run-history persistence.
#include "SqliteHistoryStore.h"

namespace chainapi::engine {

SqliteHistoryStore::SqliteHistoryStore() = default;
SqliteHistoryStore::~SqliteHistoryStore() = default;

std::expected<void, ChainApiError> SqliteHistoryStore::open(
    const std::filesystem::path& /*dbPath*/) {
    return {};
}

std::expected<void, ChainApiError> SqliteHistoryStore::append(const RunEvent& /*event*/) {
    return {};
}

std::expected<std::vector<RunEvent>, ChainApiError> SqliteHistoryStore::eventsFor(
    RunId /*run*/) const {
    return std::vector<RunEvent>{};
}

void SqliteHistoryStore::close() {}

}  // namespace chainapi::engine
