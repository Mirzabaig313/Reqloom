// SIGINT/SIGTERM (Windows: console control events) handling for `reqloom run`.

#include "RunInterrupt.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <array>
#include <cerrno>
#include <thread>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace reqloom::cli {

namespace {

// Written by the handler and read elsewhere; lock-free atomics are safe in a signal handler.
std::atomic<int> gSignal{0};
std::atomic<int> gSignalCount{0};
static_assert(std::atomic<int>::is_always_lock_free);

constexpr int kSignalExitBase = 128;

/// Records the signal; true when it is the second one and the process must exit now.
[[nodiscard]] bool recordSignal(int signal) noexcept {
    if (gSignalCount.fetch_add(1) > 0) {
        return true;
    }
    gSignal.store(signal);
    return false;
}

}  // namespace

struct RunInterrupt::State {
    explicit State(Cancel c) : cancel(std::move(c)) {}

    void onSignal() {
        const std::lock_guard lock(mutex);
        requested = true;
        if (run) {
            cancel(*run);
        }
    }

    void runStarted(engine::RunId id) {
        const std::lock_guard lock(mutex);
        run = id;
        if (requested) {
            cancel(id);
        }
    }

    Cancel cancel;
    std::mutex mutex;
    std::optional<engine::RunId> run;
    bool requested{false};

#ifndef _WIN32
    std::array<int, 2> wakePipe{-1, -1};
    struct sigaction oldInt {};
    struct sigaction oldTerm {};
    std::thread watcher;
#endif
};

#ifdef _WIN32

namespace {

// The console runs this handler on a thread of its own, so it may lock and call the engine.
std::mutex gActiveMutex;
RunInterrupt::State* gActive{nullptr};

BOOL WINAPI handleConsoleEvent(DWORD event) {
    const int signal = event == CTRL_C_EVENT ? SIGINT : SIGTERM;
    if (recordSignal(signal)) {
        std::_Exit(kSignalExitBase + signal);
    }
    const std::lock_guard lock(gActiveMutex);
    if (gActive != nullptr) {
        gActive->onSignal();
    }
    return TRUE;
}

}  // namespace

RunInterrupt::RunInterrupt(Cancel cancel) : state_(std::make_unique<State>(std::move(cancel))) {
    gSignal.store(0);
    gSignalCount.store(0);
    {
        const std::lock_guard lock(gActiveMutex);
        gActive = state_.get();
    }
    SetConsoleCtrlHandler(handleConsoleEvent, TRUE);
}

RunInterrupt::~RunInterrupt() {
    SetConsoleCtrlHandler(handleConsoleEvent, FALSE);
    const std::lock_guard lock(gActiveMutex);
    gActive = nullptr;
}

#else

namespace {

constexpr char kSignalByte = 's';
constexpr char kStopByte = 'q';
int gWakeFd{-1};

// Only async-signal-safe work here: the cancel itself happens on the watcher thread.
extern "C" void handleSignal(int signal) {
    if (recordSignal(signal)) {
        std::_Exit(kSignalExitBase + signal);
    }
    const int savedErrno = errno;
    [[maybe_unused]] const auto written = ::write(gWakeFd, &kSignalByte, 1);
    errno = savedErrno;
}

void watch(RunInterrupt::State& state, int fd) {
    for (;;) {
        char byte{};
        const auto n = ::read(fd, &byte, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0 || byte == kStopByte) {
            return;
        }
        state.onSignal();
    }
}

}  // namespace

RunInterrupt::RunInterrupt(Cancel cancel) : state_(std::make_unique<State>(std::move(cancel))) {
    gSignal.store(0);
    gSignalCount.store(0);
    auto& fds = state_->wakePipe;
    // Without a pipe the default handlers stay: the process dies on a signal as it always did.
    if (::pipe(fds.data()) != 0) {
        fds = {-1, -1};
        return;
    }
    for (const int fd : fds) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
    gWakeFd = fds[1];
    state_->watcher = std::thread(watch, std::ref(*state_), fds[0]);

    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, &state_->oldInt);
    sigaction(SIGTERM, &action, &state_->oldTerm);
}

RunInterrupt::~RunInterrupt() {
    auto& fds = state_->wakePipe;
    if (fds[0] < 0) {
        return;
    }
    sigaction(SIGINT, &state_->oldInt, nullptr);
    sigaction(SIGTERM, &state_->oldTerm, nullptr);
    [[maybe_unused]] const auto written = ::write(fds[1], &kStopByte, 1);
    state_->watcher.join();
    gWakeFd = -1;
    ::close(fds[0]);
    ::close(fds[1]);
}

#endif

void RunInterrupt::runStarted(engine::RunId run) {
    state_->runStarted(run);
}

int RunInterrupt::exitCode() const noexcept {
    const int signal = gSignal.load();
    return signal == 0 ? 0 : kSignalExitBase + signal;
}

}  // namespace reqloom::cli
