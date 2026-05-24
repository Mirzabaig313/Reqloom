// DependencyResolver — Engine Requirement §3.1, §4.
//
// Skeleton: real topological sort + cycle detection lands in Phase 1.
// Surface preserved so application/use-case code can be wired now.
#include "DependencyResolver.h"

namespace chainapi::engine {

DependencyResolver::DependencyResolver() = default;
DependencyResolver::~DependencyResolver() = default;

std::expected<std::vector<OperationId>, ChainApiError>
DependencyResolver::resolve(const Project& /*project*/,
                            const OperationId& target) const {
    // Phase 1 will:
    //   1. Build implicit edges from `{{resource.field}}` references.
    //   2. Union with explicit `depends_on` edges.
    //   3. Detect cycles (Engine Req AC-3.1.4 / AC-3.1.5).
    //   4. Stable topological sort (lexicographic tie-break — AC-3.1.3).
    return std::vector<OperationId>{ target };
}

}  // namespace chainapi::engine
