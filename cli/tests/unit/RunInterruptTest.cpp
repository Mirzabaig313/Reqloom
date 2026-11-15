// Tests for SIGINT/SIGTERM handling during `reqloom run`: the run is cancelled through the
// engine from a normal thread, and the exit code names the signal.

#include "commands/RunInterrupt.h"

#include <reqloom/engine/Events.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#ifndef _WIN32

namespace cli = reqloom::cli;
namespace ce = reqloom::engine;

namespace {

/// Records the run ids passed to cancel.
class CancelLog {
public:
    void operator()(ce::RunId run) {
        const std::lock_guard lock(mutex_);
        ids_.push_back(run.value);
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<std::uint64_t> waitForOne() {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds{5}, [this] { return !ids_.empty(); });
        return ids_;
    }

    [[nodiscard]] std::vector<std::uint64_t> snapshot() {
        const std::lock_guard lock(mutex_);
        return ids_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::uint64_t> ids_;
};

}  // namespace

TEST(RunInterrupt, sigint_during_a_run_cancels_it_and_exits_130) {
    CancelLog log;
    cli::RunInterrupt interrupt{[&log](ce::RunId id) {
        log(id);
    }};
    interrupt.runStarted(ce::RunId{5});
    EXPECT_EQ(interrupt.exitCode(), 0);

    ASSERT_EQ(std::raise(SIGINT), 0);

    EXPECT_EQ(log.waitForOne(), std::vector<std::uint64_t>{5});
    EXPECT_EQ(interrupt.exitCode(), 130);
}

TEST(RunInterrupt, sigterm_before_the_run_starts_cancels_it_once_announced) {
    CancelLog log;
    cli::RunInterrupt interrupt{[&log](ce::RunId id) {
        log(id);
    }};

    ASSERT_EQ(std::raise(SIGTERM), 0);
    EXPECT_EQ(interrupt.exitCode(), 143);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    interrupt.runStarted(ce::RunId{7});

    EXPECT_EQ(log.waitForOne(), std::vector<std::uint64_t>{7});
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_EQ(log.snapshot(), std::vector<std::uint64_t>{7}) << "cancelled exactly once";
}

TEST(RunInterrupt, previous_handlers_are_restored) {
    struct sigaction before {};
    ASSERT_EQ(sigaction(SIGINT, nullptr, &before), 0);
    {
        const cli::RunInterrupt interrupt{[](ce::RunId) {
        }};
        struct sigaction during {};
        ASSERT_EQ(sigaction(SIGINT, nullptr, &during), 0);
        EXPECT_NE(during.sa_handler, before.sa_handler);
    }
    struct sigaction after {};
    ASSERT_EQ(sigaction(SIGINT, nullptr, &after), 0);
    EXPECT_EQ(after.sa_handler, before.sa_handler);
}

TEST(RunInterruptDeathTest, second_signal_exits_immediately) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            const cli::RunInterrupt interrupt{[](ce::RunId) {
            }};
            std::ignore = std::raise(SIGINT);
            std::ignore = std::raise(SIGINT);
            std::this_thread::sleep_for(std::chrono::seconds{10});
            std::exit(0);
        },
        testing::ExitedWithCode(130),
        "");
}

#endif
