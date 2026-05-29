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

    /// Whole-project validation, run at schema load so a malformed
    /// project is rejected before any operation can run:
    ///   - every `{{X.y}}` reference names a known scope ($ builtin,
    ///     env, secret, a defined actor or resource) → else RefUndefined
    ///     (field existence stays a runtime concern);
    ///   - every `depends_on:` target names a real operation → else
    ///     RefUndefined;
    ///   - the full dependency graph is acyclic, self-loops included →
    ///     else Cycle, listing the operations involved.
    [[nodiscard]] std::expected<void, ChainApiError> validate(const Project& project) const;

    /// Collect the distinct `secret.X` names referenced anywhere in the
    /// project (operation templates, actor auth config, auth steps,
    /// refresh blocks, poll predicates). Used at run start to pre-load
    /// exactly those secrets from the SecretStore into the resolve
    /// context — we never bulk-dump the keychain. Sorted, de-duplicated.
    [[nodiscard]] static std::vector<std::string> collectSecretReferences(const Project& project);
};

}  // namespace chainapi::engine
