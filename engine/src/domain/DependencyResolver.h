// Engine-internal: builds the execution chain for a target operation.
// Engine Requirement §3.1.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>
#include <chainapi/engine/Operation.h>

#include <expected>
#include <vector>

namespace chainapi::engine {

class DependencyResolver {
public:
    DependencyResolver();
    ~DependencyResolver();

    /// Returns the chain in topological order, terminating with `target`.
    /// Returns `ChainApiError{Cycle | RefUndefined | ...}` on schema
    /// problems detected during resolution.
    std::expected<std::vector<OperationId>, ChainApiError> resolve(
        const Project& project,
        const OperationId& target) const;
};

}  // namespace chainapi::engine
