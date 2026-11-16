#pragma once

#include <reqloom/engine/PublicApi.h>

#include <memory>

namespace reqloom::desktop {

/// Constructs an ExecutionEngine with concrete infrastructure and exposes it to the desktop.
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

}  // namespace reqloom::desktop
