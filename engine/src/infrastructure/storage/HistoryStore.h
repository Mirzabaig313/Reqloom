// Engine-internal interface for persisting run logs.
// Concrete impl: SqliteHistoryStore.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>

#include <expected>
#include <filesystem>
#include <vector>

namespace chainapi::engine {

class HistoryStore {
public:
    virtual ~HistoryStore() = default;

    virtual std::expected<void, ChainApiError> open(const std::filesystem::path& dbPath) = 0;
    virtual std::expected<void, ChainApiError> append(const RunEvent& event) = 0;
    virtual std::expected<std::vector<RunEvent>, ChainApiError> eventsFor(RunId run) const = 0;
    virtual void close() = 0;
};

}  // namespace chainapi::engine
