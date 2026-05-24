// VariableResolver — substitutes {{X.y}} references per PRD §5.7 and
// Engine Req §3.10.
//
// Resolution order (first match wins):
//   1. Builtins ($.now, $.uuid, $.faker.*, $.env.*)
//   2. Actor sessions
//   3. Resource extractions (most-recent or indexed)
//   4. Environment variables
//   5. Secrets
#include "VariableResolver.h"

namespace chainapi::engine {

VariableResolver::Result VariableResolver::resolve(std::string_view /*templateStr*/,
                                                   const RunContext& /*ctx*/) const {
    // Phase 1: real templating engine. Skeleton returns empty success.
    return Result{"", {}};
}

}  // namespace chainapi::engine
