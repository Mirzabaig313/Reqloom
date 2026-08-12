// ExecutionPreview — the resolved execution path for a target operation, shaped
// for display before anything has run. Turns an engine ResolvedPlan plus the
// Project into an ordered list of steps with the variables each one produces, so
// the response pane can show what a run is *about to* do instead of an empty
// placeholder.
//
// Pure: no I/O, no Qt widgets, no engine calls. The caller resolves the plan
// (ExecutionEngine::resolvePlan) and passes it in, which keeps this unit-testable
// against an in-memory Project.
//
// Path templates are reported verbatim, with `{{variable}}` references intact
// and unresolved. That is deliberate: an unresolved template cannot leak a
// secret, whereas a resolved URL would need masking before display.
#pragma once

#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace reqloom::desktop {

/// One value a step extracts from its response, and where it comes from.
struct PreviewOutput {
    /// Fully qualified as `<resource>.<name>` — the form a template must use.
    ///
    /// The schema stores extraction names bare (`order_id`), while references and
    /// dependency edges use the qualified form (`orders.order_id`). Displaying the
    /// bare name would show a variable that does not resolve if copied, so the
    /// resource prefix is applied here once.
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
///
/// Steps appear in the plan's topological order, so the target is last. A step
/// whose operation is absent from `project` still appears, carrying only its id
/// — a missing operation is exactly the case worth showing rather than hiding.
///
/// Unlike the chain *graph*, a single-step plan is not a special case: an
/// operation with no dependencies previews as one step.
[[nodiscard]] std::vector<PreviewStep> buildExecutionPreview(const engine::ResolvedPlan& plan,
                                                             const engine::Project& project);

/// Operations in `plan` that consume `variable` produced by `producerOperationId`,
/// sorted. Answers "what breaks when this extraction comes back empty".
///
/// `variable` is accepted either bare (`order_id`, as extraction events report it)
/// or already qualified (`orders.order_id`); it is normalised against the
/// producer's resource before matching, so a caller cannot silently get an empty
/// result from passing the wrong form.
///
/// Reads the plan's edges only — the engine already tags each implicit edge with
/// the variable that flows along it, so nothing is re-derived here.
[[nodiscard]] QStringList consumersOfVariable(const engine::ResolvedPlan& plan,
                                              const QString& producerOperationId,
                                              const QString& variable);

}  // namespace reqloom::desktop
