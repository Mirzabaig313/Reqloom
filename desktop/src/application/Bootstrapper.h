#pragma once

#include <chainapi/engine/PublicApi.h>

#include <memory>

namespace chainapi::desktop {

/// Constructs an ExecutionEngine with concrete infrastructure implementations
/// and exposes it to the rest of the desktop. Centralising the wiring here
/// keeps view models free of dependency-injection logic.
class Bootstrapper {
public:
    Bootstrapper();
    Bootstrapper(const Bootstrapper&) = delete;
    Bootstrapper& operator=(const Bootstrapper&) = delete;
    Bootstrapper(Bootstrapper&&) = delete;
    Bootstrapper& operator=(Bootstrapper&&) = delete;
    ~Bootstrapper();

    engine::ExecutionEngine& engine() noexcept;

private:
    std::unique_ptr<engine::ExecutionEngine> engine_;
};

}  // namespace chainapi::desktop
