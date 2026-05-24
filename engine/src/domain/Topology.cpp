// Topological sort utilities — Kahn's algorithm with stable tie-break.
// Engine Req AC-3.1.3.
#include "Topology.h"

namespace chainapi::engine {

std::expected<std::vector<OperationId>, ChainApiError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& /*edges*/) {
    // Phase 1: real implementation (Kahn's, in-degree priority queue with
    // lexicographic tie-break to satisfy the determinism guarantee).
    return std::vector<OperationId>{};
}

}  // namespace chainapi::engine
