// Engine-internal: builds the execution chain for a target operation.
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

    /// Returns the chain in topological order, terminating with `target`.
    /// Returns `ChainApiError{Cycle | RefUndefined | ...}` on schema
    /// problems detected during resolution.
    [[nodiscard]] std::expected<std::vector<OperationId>, ChainApiError> resolve(
        const Project& project, const OperationId& target) const;

    /// Whole-project static validation, run at schema load time.
    ///
    /// Enforces two load-time contracts so a malformed project is
    /// rejected before any operation can run 
    ///   - Every `{{X.y}}` reference names a known scope: `$` builtins,
    ///     `env`, `secret`, a defined actor, or a defined resource.
    ///     Unknown scope → `RefUndefined`. (Field existence is a
    ///     runtime concern — a real resource with a typo'd field still
    ///     loads and surfaces the miss at run time.)
    ///   - Every `depends_on:` target names an existing operation.
    ///     Missing target → `RefUndefined`.
    ///   - The full dependency graph (explicit + implicit edges across
    ///     all operations) is acyclic. A cycle, including a one-node
    ///     self-loop, → `Cycle` listing the operations involved.
    [[nodiscard]] std::expected<void, ChainApiError> validate(const Project& project) const;
};

}  // namespace chainapi::engine
