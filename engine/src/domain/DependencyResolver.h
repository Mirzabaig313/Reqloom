// Engine-internal: builds the execution chain for a target operation.
#pragma once

#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/VariableSuggestion.h>

#include <expected>
#include <string>
#include <vector>

namespace reqloom::engine {

class DependencyResolver {
public:
    DependencyResolver();

    /// Returns the chain in topological order, terminating with `target`.
    /// Returns `ReqloomError{Cycle | RefUndefined | ...}` on schema
    /// problems detected during resolution.
    [[nodiscard]] std::expected<std::vector<OperationId>, ReqloomError> resolve(
        const Project& project, const OperationId& target) const;

    /// Like `resolve`, but additionally returns the resolved dependency
    /// edges (explicit + implicit, with the flowing variable for implicit
    /// ones). Used to draw the real resolved chain graph. `resolve` is a
    /// thin wrapper over this that discards the edges.
    [[nodiscard]] std::expected<ResolvedPlan, ReqloomError> resolvePlan(
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
    [[nodiscard]] std::expected<void, ReqloomError> validate(const Project& project) const;

    /// Collect the distinct `secret.X` names referenced anywhere in the
    /// project (operation templates, actor auth config, auth steps,
    /// refresh blocks, poll predicates). Used at run start to pre-load
    /// exactly those secrets from the SecretStore into the resolve
    /// context — we never bulk-dump the keychain. Sorted, de-duplicated.
    [[nodiscard]] static std::vector<std::string> collectSecretReferences(const Project& project);
};

/// Enumerate the `{{...}}` references usable at `target`, given its already
/// resolved `plan`: upstream extracts (`resource.var` for every producing op
/// in the chain), the target actor's session tokens (`actor.var`), the chosen
/// environment's vars (`env.X`), declared secrets (`secret.X`), and `$.`
/// built-ins. De-duplicated; `environment` empty → the project default. Pure
/// (no I/O) so the editor can call it on every keystroke.
[[nodiscard]] std::vector<VariableSuggestion> collectVariableSuggestions(
    const Project& project,
    const OperationId& target,
    const ResolvedPlan& plan,
    const std::string& environment);

}  // namespace reqloom::engine
