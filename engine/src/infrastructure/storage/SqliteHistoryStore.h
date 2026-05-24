#pragma once

#include "HistoryStore.h"

namespace chainapi::engine {

class SqliteHistoryStore final : public HistoryStore {
public:
    SqliteHistoryStore();
    ~SqliteHistoryStore() override;

    std::expected<void, ChainApiError> open(const std::filesystem::path& dbPath) override;
    std::expected<void, ChainApiError> append(const RunEvent& event) override;
    std::expected<std::vector<RunEvent>, ChainApiError> eventsFor(RunId run) const override;
    void close() override;
};

}  // namespace chainapi::engine
