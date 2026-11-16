// SIGINT/SIGTERM (Windows: console control events) handling for `reqloom run`.
#pragma once

#include <reqloom/engine/Events.h>

#include <functional>
#include <memory>

namespace reqloom::cli {

/// While alive, the first SIGINT or SIGTERM cancels the run through `cancel`, called from a
/// normal thread once the run id is known; a second signal exits the process at once with the
/// signal's code. Restores the previous handlers on destruction. One instance at a time.
class RunInterrupt {
public:
    using Cancel = std::function<void(engine::RunId)>;

    explicit RunInterrupt(Cancel cancel);
    ~RunInterrupt();

    RunInterrupt(const RunInterrupt&) = delete;
    RunInterrupt& operator=(const RunInterrupt&) = delete;
    RunInterrupt(RunInterrupt&&) = delete;
    RunInterrupt& operator=(RunInterrupt&&) = delete;

    /// The run to cancel; cancels it at once if a signal already arrived.
    void runStarted(engine::RunId run);

    /// 0 when no signal arrived, else 130 for SIGINT and 143 for SIGTERM.
    [[nodiscard]] int exitCode() const noexcept;

    struct State;

private:
    std::unique_ptr<State> state_;
};

}  // namespace reqloom::cli
