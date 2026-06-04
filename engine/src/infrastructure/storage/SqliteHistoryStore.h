#pragma once

#include "HistoryStore.h"

#include <memory>

namespace reqloom::engine {

class SqliteHistoryStore final : public HistoryStore {
public:
    SqliteHistoryStore();
    SqliteHistoryStore(const SqliteHistoryStore&) = delete;
    SqliteHistoryStore& operator=(const SqliteHistoryStore&) = delete;
    SqliteHistoryStore(SqliteHistoryStore&&) = delete;
    SqliteHistoryStore& operator=(SqliteHistoryStore&&) = delete;
    ~SqliteHistoryStore() override;

    std::expected<void, ReqloomError> open(const std::filesystem::path& dbPath) override;
    std::expected<void, ReqloomError> append(const RunEvent& event) override;
    [[nodiscard]] std::expected<std::vector<RunEvent>, ReqloomError> eventsFor(
        RunId run) const override;
    [[nodiscard]] std::expected<std::vector<RunHistoryRow>, ReqloomError> listRuns(
        std::size_t limit) const override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace reqloom::engine
