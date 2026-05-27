// Topological sort utilities — Kahn's algorithm with stable tie-break.
//
// Edge semantics: `edges[node] = [deps...]` means `node` depends on each
// element of `deps`. The sort places dependencies before dependents, so
// the target operation (the one that depends on everything else) comes
// last. Tie-break is lexicographic over `OperationId.value` to keep
// runs deterministic across hash-map iteration order.
#include "Topology.h"

#include <queue>
#include <set>

namespace chainapi::engine {

std::expected<std::vector<OperationId>, ChainApiError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& edges) {
    // Empty graph trivially satisfies any topological order.
    if (edges.empty()) {
        return std::vector<OperationId>{};
    }

    // Build the in-degree table and reverse-edge map. Nodes that appear
    // only as dependency targets (never as a key in `edges`) still need
    // a zero in-degree entry so they're picked up by the ready queue.
    std::map<OperationId, int> inDegree;
    std::map<OperationId, std::vector<OperationId>> dependents;

    for (const auto& [node, deps] : edges) {
        if (!inDegree.contains(node)) {
            inDegree[node] = 0;
        }
        for (const auto& dep : deps) {
            dependents[dep].push_back(node);
            ++inDegree[node];
            if (!inDegree.contains(dep)) {
                inDegree[dep] = 0;
            }
        }
    }

    // Min-heap on OperationId.value gives lexicographic tie-break for
    // independent nodes. Comparator inverts the natural order because
    // std::priority_queue is a max-heap by default.
    auto cmp = [](const OperationId& a, const OperationId& b) {
        return a.value > b.value;
    };
    std::priority_queue<OperationId, std::vector<OperationId>, decltype(cmp)> ready(cmp);

    for (const auto& [node, degree] : inDegree) {
        if (degree == 0) {
            ready.push(node);
        }
    }

    std::vector<OperationId> sorted;
    sorted.reserve(inDegree.size());

    while (!ready.empty()) {
        auto node = ready.top();
        ready.pop();
        sorted.push_back(node);

        if (auto it = dependents.find(node); it != dependents.end()) {
            for (const auto& dependent : it->second) {
                if (--inDegree[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
    }

    // Cycle detection: if any node never reached zero in-degree, the
    // remaining nodes form (or feed into) a cycle.
    if (sorted.size() < inDegree.size()) {
        std::set<OperationId> sortedSet(sorted.begin(), sorted.end());
        std::string cycleOps;
        for (const auto& [node, _] : inDegree) {
            if (!sortedSet.contains(node)) {
                if (!cycleOps.empty()) {
                    cycleOps += " → ";
                }
                cycleOps += node.value;
            }
        }
        return std::unexpected(ChainApiError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    return sorted;
}

}  // namespace chainapi::engine
