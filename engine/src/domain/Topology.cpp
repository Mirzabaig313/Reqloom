// Topological sort utilities — Kahn's algorithm with stable tie-break.
// `edges[node]` lists node's dependencies, which sort first; ties break lexicographically.
#include "Topology.h"

#include <queue>
#include <set>

namespace reqloom::engine {

std::expected<std::vector<OperationId>, ReqloomError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& edges) {
    if (edges.empty()) {
        return std::vector<OperationId>{};
    }

    // Nodes seen only as dependencies still need a zero in-degree entry.
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

    // Min-heap (inverted comparator, as priority_queue is a max-heap) gives the lexicographic
    // tie-break.
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

    // Nodes that never reached zero in-degree form or feed a cycle.
    if (sorted.size() < inDegree.size()) {
        std::set<OperationId> const sortedSet(sorted.begin(), sorted.end());
        std::string cycleOps;
        for (const auto& [node, _] : inDegree) {
            if (!sortedSet.contains(node)) {
                if (!cycleOps.empty()) {
                    cycleOps += " → ";
                }
                cycleOps += node.value;
            }
        }
        return std::unexpected(ReqloomError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    return sorted;
}

}  // namespace reqloom::engine
