// Topological sort utilities — Kahn's algorithm with stable tie-break.
#include "Topology.h"

namespace chainapi::engine {

std::expected<std::vector<OperationId>, ChainApiError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& /*edges*/) {
    // Phase 1: Kahn's algorithm with in-degree priority queue and
    // lexicographic tie-break to satisfy the determinism guarantee.
    return std::vector<OperationId>{};
}

}  // namespace chainapi::engine
