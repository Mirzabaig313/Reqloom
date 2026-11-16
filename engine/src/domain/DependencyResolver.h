// Engine-internal: builds the execution chain for a target operation.
#pragma once

#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/VariableSuggestion.h>

#include <expected>
#include <string>
#include <string_view>
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

    /// Like `resolve`, but also returns the resolved edges (explicit + implicit, with the flowing
    /// variable for implicit ones). `resolve` wraps this.
    [[nodiscard]] std::expected<ResolvedPlan, ReqloomError> resolvePlan(
        const Project& project, const OperationId& target) const;

    /// Whole-project validation, run at schema load. `RefUndefined` for an unknown `{{X.y}}` scope,
    /// an unknown `depends_on` / `actor` / `poll_until.actor` / `for_each.over` target, or a
    /// `{{res.var}}` with no or an ambiguous producer (extractors that also consume it are skipped,
    /// `depends_on` disambiguates). `Cycle`, listing the operations involved, when the graph is
    /// cyclic (self-loops included).
    [[nodiscard]] std::expected<void, ReqloomError> validate(const Project& project) const;

    /// The distinct `secret.X` names referenced anywhere in the project, sorted; used to pre-load
    /// exactly those secrets rather than bulk-dump the keychain.
    [[nodiscard]] static std::vector<std::string> collectSecretReferences(const Project& project);
};

/// Check that an actor id, resource id or environment name (`kind`) can be a file stem: not
/// empty, no leading `.`, no `/`, `\`, `:` or NUL. Used at load (`validate`) and by the writer.
///
/// @return void, or `SchemaInvalid` naming `kind` and `name`.
[[nodiscard]] std::expected<void, ReqloomError> checkEntityFileStem(std::string_view kind,
                                                                    std::string_view name);

/// Enumerate the `{{...}}` references usable at `target` given its resolved `plan`: upstream
/// extracts, the actor's session tokens, env vars, declared secrets and `$.` built-ins.
/// De-duplicated; `environment` empty means the project default. Pure, so the editor can call it on
/// every keystroke.
[[nodiscard]] std::vector<VariableSuggestion> collectVariableSuggestions(
    const Project& project,
    const OperationId& target,
    const ResolvedPlan& plan,
    const std::string& environment);

}  // namespace reqloom::engine
