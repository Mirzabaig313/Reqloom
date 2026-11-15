// Constructs the ExecutionEngine with default infrastructure, keeping wiring out of the view
// models.
#include "Bootstrapper.h"

#include <reqloom/engine/Factories.h>

namespace reqloom::desktop {

// The run-history database is opened per project by AppController, not here.
Bootstrapper::Bootstrapper()
    : engine_(std::make_unique<engine::ExecutionEngine>(engine::makeDefaultDependencies())) {}

Bootstrapper::~Bootstrapper() = default;

engine::ExecutionEngine& Bootstrapper::engine() noexcept {
    return *engine_;
}

}  // namespace reqloom::desktop
