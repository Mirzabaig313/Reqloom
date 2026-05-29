#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>

#include <expected>
#include <map>
#include <vector>

namespace chainapi::engine {

/// Stable topological sort. Tie-break is lexicographic over `OperationId.value`.
/// Returns `ChainApiError{Cycle, ...}` if the graph contains a cycle.
[[nodiscard]] std::expected<std::vector<OperationId>, ChainApiError> topologicalSort(
    const std::map<OperationId, std::vector<OperationId>>& edges);

}  // namespace chainapi::engine
