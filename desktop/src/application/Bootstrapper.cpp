// Constructs the ExecutionEngine with default infrastructure. Centralising
// the wiring keeps view models free of dependency-injection logic.
#include "Bootstrapper.h"

#include <reqloom/engine/Factories.h>

namespace reqloom::desktop {

// Note: the run-history database is opened per-project by AppController when a
// project loads (so runs are isolated between projects), not globally here.
Bootstrapper::Bootstrapper()
    : engine_(std::make_unique<engine::ExecutionEngine>(engine::makeDefaultDependencies())) {}

Bootstrapper::~Bootstrapper() = default;

engine::ExecutionEngine& Bootstrapper::engine() noexcept {
    return *engine_;
}

}  // namespace reqloom::desktop
