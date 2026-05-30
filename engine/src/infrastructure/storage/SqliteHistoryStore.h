#pragma once

#include "HistoryStore.h"

#include <memory>

namespace chainapi::engine {

class SqliteHistoryStore final : public HistoryStore {
public:
    SqliteHistoryStore();
    SqliteHistoryStore(const SqliteHistoryStore&) = delete;
    SqliteHistoryStore& operator=(const SqliteHistoryStore&) = delete;
    SqliteHistoryStore(SqliteHistoryStore&&) = delete;
    SqliteHistoryStore& operator=(SqliteHistoryStore&&) = delete;
    ~SqliteHistoryStore() override;

    std::expected<void, ChainApiError> open(const std::filesystem::path& dbPath) override;
    std::expected<void, ChainApiError> append(const RunEvent& event) override;
    [[nodiscard]] std::expected<std::vector<RunEvent>, ChainApiError> eventsFor(
        RunId run) const override;
    [[nodiscard]] std::expected<std::vector<RunHistoryRow>, ChainApiError> listRuns(
        std::size_t limit) const override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chainapi::engine
