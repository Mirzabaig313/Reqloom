#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Operation.h>

#include <expected>
#include <map>
#include <vector>

namespace reqloom::engine {

/// Stable topological sort. Tie-break is lexicographic over `OperationId.value`.
/// Returns `ReqloomError{Cycle, ...}` if the graph contains a cycle.
[[nodiscard]] std::expected<std::vector<OperationId>, ReqloomError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& edges);

}  // namespace reqloom::engine
