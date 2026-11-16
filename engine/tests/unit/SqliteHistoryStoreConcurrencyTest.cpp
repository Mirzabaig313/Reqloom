// SqliteHistoryStore shared between threads: the engine worker appends while the desktop GUI thread
// lists, reads and clears through the same store.
#include "infrastructure/storage/SqliteHistoryStore.h"

#include <reqloom/engine/Events.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class TempDb {
public:
    TempDb() { path_ = reqloom::tests::uniqueTempPath("reqloom-history-mt", ".sqlite"); }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path_, ec);
        fs::remove(fs::path{path_.string() + "-wal"}, ec);
        fs::remove(fs::path{path_.string() + "-shm"}, ec);
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] ce::RunStarted runStarted(std::uint64_t id) {
    ce::RunStarted started;
    started.runId = ce::RunId{id};
    started.target = ce::OperationId{"order.create"};
    started.chainSize = 2;
    started.envName = "local";
    started.at = std::chrono::system_clock::now();
    return started;
}

[[nodiscard]] ce::StepStarted stepStarted(std::uint64_t id, std::size_t index) {
    ce::StepStarted step;
    step.runId = ce::RunId{id};
    step.stepIndex = index;
    step.op = ce::OperationId{"order.create"};
    step.attempt = 1;
    step.at = std::chrono::system_clock::now();
    return step;
}

}  // namespace

TEST(SqliteHistoryStoreConcurrency, appends_while_another_thread_reads_and_clears_all_succeed) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    constexpr std::uint64_t kRuns = 150;
    constexpr std::size_t kStepsPerRun = 6;

    std::atomic_bool writerDone{false};
    std::atomic<int> appendFailures{0};
    std::thread writer{[&] {
        for (std::uint64_t id = 1; id <= kRuns; ++id) {
            if (!store.append(runStarted(id)).has_value()) {
                ++appendFailures;
            }
            for (std::size_t i = 0; i < kStepsPerRun; ++i) {
                if (!store.append(stepStarted(id, i)).has_value()) {
                    ++appendFailures;
                }
            }
        }
        writerDone.store(true);
    }};

    int readFailures{0};
    int iteration{0};
    while (!writerDone.load()) {
        if (!store.listRuns(20).has_value()) {
            ++readFailures;
        }
        if (!store.eventsFor(ce::RunId{static_cast<std::uint64_t>(iteration % 50) + 1})
                 .has_value()) {
            ++readFailures;
        }
        if (++iteration % 25 == 0 && !store.clear().has_value()) {
            ++readFailures;
        }
    }
    writer.join();

    EXPECT_EQ(appendFailures.load(), 0);
    EXPECT_EQ(readFailures, 0);
    const auto runs = store.listRuns(0);
    ASSERT_TRUE(runs.has_value());
}
