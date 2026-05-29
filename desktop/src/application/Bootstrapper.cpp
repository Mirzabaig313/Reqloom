#include "Bootstrapper.h"

namespace chainapi::desktop {

// Wire concrete dependencies into ExecutionEngine via the factory helpers:
//
//   engine::ExecutionEngine::Dependencies deps {
//       engine::makeCurlHttpClient(),
//       engine::makeYamlSchemaParser(),
//       engine::makeSqliteHistoryStore(),
//       engine::makeKeychainSecretStore(),
//       engine::makeQuickJsHookRunner(),
//   };
//   engine_ = std::make_unique<engine::ExecutionEngine>(std::move(deps));
Bootstrapper::Bootstrapper() = default;

Bootstrapper::~Bootstrapper() = default;

engine::ExecutionEngine& Bootstrapper::engine() noexcept {
    return *engine_;
}

}  // namespace chainapi::desktop
