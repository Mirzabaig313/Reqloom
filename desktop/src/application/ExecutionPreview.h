// ExecutionPreview — the resolved execution path for a target operation, shaped for display before
// anything runs. Pure (no I/O or engine calls): the caller resolves the plan. Path templates stay
// unresolved, so no secret can leak.
#pragma once

#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace reqloom::desktop {

/// One value a step extracts from its response, and where it comes from.
struct PreviewOutput {
    /// Fully qualified as `<resource>.<name>`, the form a template must use (the schema stores
    /// extraction names bare).
    QString variable;
    QString sourcePath;  ///< JSONPath / header name / regex, per the source kind.
};

/// One step of a resolved chain, in execution order.
struct PreviewStep {
    int number{0};  ///< 1-based position in the execution order.
    QString operationId;
    QString method;        ///< Uppercase verb, empty when the operation is missing.
    QString pathTemplate;  ///< `{{...}}` left unresolved on purpose (see header note).
    QString actor;
    bool isTarget{false};   ///< The operation the user asked to run: the last step.
    QStringList dependsOn;  ///< Producer operation ids, sorted for stable display.
    std::vector<PreviewOutput> produces;
    QList<int> expectStatus;  ///< Declared expected status codes; empty when unset.
};

/// Build the display model for `plan`, resolving each step against `project`.
/// Steps follow the plan's topological order (target last); a step missing from `project` still
/// appears with only its id.
[[nodiscard]] std::vector<PreviewStep> buildExecutionPreview(const engine::ResolvedPlan& plan,
                                                             const engine::Project& project);

/// Operations in `plan` that consume `variable` produced by `producerOperationId`, sorted.
/// `variable` may be bare or qualified; it is normalised against the producer's resource. Reads the
/// plan's edges only.
[[nodiscard]] QStringList consumersOfVariable(const engine::ResolvedPlan& plan,
                                              const QString& producerOperationId,
                                              const QString& variable);

}  // namespace reqloom::desktop
