#include "Bootstrapper.h"

namespace chainapi::desktop {

Bootstrapper::Bootstrapper() {
    // Phase 1/2: construct concrete dependencies (CurlHttpClient,
    // YamlSchemaParser, SqliteHistoryStore, KeychainSecretStore,
    // QuickJsHookRunner) and inject them into ExecutionEngine.
    //
    // Those concrete types live in engine/src/ and are not part of the
    // public engine surface, so the Bootstrapper depends on factory
    // helpers exposed from the engine library (added when the concrete
    // implementations land):
    //
    //   engine::ExecutionEngine::Dependencies deps {
    //       engine::makeCurlHttpClient(),
    //       engine::makeYamlSchemaParser(),
    //       engine::makeSqliteHistoryStore(),
    //       engine::makeKeychainSecretStore(),
    //       engine::makeQuickJsHookRunner(),
    //   };
    //   engine_ = std::make_unique<engine::ExecutionEngine>(std::move(deps));
}

Bootstrapper::~Bootstrapper() = default;

engine::ExecutionEngine& Bootstrapper::engine() noexcept {
    return *engine_;
}

}  // namespace chainapi::desktop
