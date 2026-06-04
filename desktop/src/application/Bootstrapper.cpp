// Constructs the ExecutionEngine with default infrastructure. Centralising
// the wiring keeps view models free of dependency-injection logic.
#include "Bootstrapper.h"

#include <reqloom/engine/Factories.h>

namespace reqloom::desktop {

Bootstrapper::Bootstrapper()
    : engine_(std::make_unique<engine::ExecutionEngine>(engine::makeDefaultDependencies())) {}

Bootstrapper::~Bootstrapper() = default;

engine::ExecutionEngine& Bootstrapper::engine() noexcept {
    return *engine_;
}

}  // namespace reqloom::desktop
