// ExecutionPreview — see header.
#include "ExecutionPreview.h"

#include "views/Formatting.h"

#include <algorithm>
#include <map>
#include <set>

namespace reqloom::desktop {

namespace {

/// Locate an operation by its "<resource>.<op_name>" id. Returns nullptr when
/// either half is unknown, which callers render as a missing step.
[[nodiscard]] const engine::Operation* findOperation(const engine::Project& project,
                                                     const std::string& id) {
    const std::size_t dot = id.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= id.size()) {
        return nullptr;
    }
    const auto resourceIt = project.resources.find(engine::ResourceId{id.substr(0, dot)});
    if (resourceIt == project.resources.end()) {
        return nullptr;
    }
    const auto opIt = resourceIt->second.operations.find(id.substr(dot + 1));
    return opIt == resourceIt->second.operations.end() ? nullptr : &opIt->second;
}

/// Resource half of an operation id ("orders.create" → "orders"), which is also
/// the namespace its extracted variables are referenced under. Empty when the id
/// has no resource half.
[[nodiscard]] QString resourceOf(const QString& operationId) {
    const qsizetype dot = operationId.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? operationId.left(dot) : QString{};
}

/// Namespace an extraction name the way a template must reference it:
/// `<producing resource>.<stored key>`.
///
/// The prefix is applied even when the stored key itself contains a dot. An
/// extraction declared as `employer.org_id` on `admin_organization.list` is
/// stored as the literal key "employer.org_id" inside resource
/// `admin_organization`, and `substituteRef` splits a reference at its FIRST dot
/// only — so the reference that reaches it is
/// `{{admin_organization.employer.org_id}}`. Skipping the prefix for dotted keys
/// would print `{{employer.org_id}}`, which resolves against something else
/// entirely (an `employer` actor's session, if one exists) or not at all.
[[nodiscard]] QString qualifyVariable(const QString& resource, const QString& variableName) {
    if (resource.isEmpty()) {
        return variableName;
    }
    return resource + QLatin1Char('.') + variableName;
}

/// Producer ids per consumer, de-duplicated: an explicit `depends_on` and an
/// implicit `{{var}}` reference between the same pair are one dependency to a
/// reader, even though the plan carries them as separate edges.
[[nodiscard]] std::map<std::string, std::set<std::string>> producersByConsumer(
    const engine::ResolvedPlan& plan) {
    std::map<std::string, std::set<std::string>> producers;
    for (const auto& edge : plan.edges) {
        producers[edge.consumer.value].insert(edge.producer.value);
    }
    return producers;
}

/// Declared expected status codes. The multi-value `expect_status: [200, 202]`
/// form takes precedence over the singular field, matching the engine.
[[nodiscard]] QList<int> expectedStatuses(const engine::Operation& op) {
    QList<int> statuses;
    if (!op.expectStatusList.empty()) {
        statuses.reserve(static_cast<qsizetype>(op.expectStatusList.size()));
        for (const int status : op.expectStatusList) {
            statuses.append(status);
        }
        return statuses;
    }
    if (op.expectStatus.has_value()) {
        statuses.append(*op.expectStatus);
    }
    return statuses;
}

}  // namespace

std::vector<PreviewStep> buildExecutionPreview(const engine::ResolvedPlan& plan,
                                               const engine::Project& project) {
    const auto producers = producersByConsumer(plan);

    std::vector<PreviewStep> steps;
    steps.reserve(plan.order.size());

    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const std::string& id = plan.order[i].value;

        PreviewStep step;
        step.number = static_cast<int>(i) + 1;
        step.operationId = QString::fromStdString(id);
        // The plan is ordered dependencies-first, so the target is last.
        step.isTarget = (i + 1 == plan.order.size());

        if (const auto producerIt = producers.find(id); producerIt != producers.end()) {
            step.dependsOn.reserve(static_cast<qsizetype>(producerIt->second.size()));
            for (const std::string& producer : producerIt->second) {
                step.dependsOn.append(QString::fromStdString(producer));
            }
            // std::set already orders these; sort() keeps the guarantee explicit
            // for readers and survives a change of container.
            step.dependsOn.sort();
        }

        if (const engine::Operation* op = findOperation(project, id); op != nullptr) {
            step.method = format::method(op->method);
            step.pathTemplate = QString::fromStdString(op->pathTemplate);
            step.actor = QString::fromStdString(op->actor.value);
            step.expectStatus = expectedStatuses(*op);
            const QString resource = resourceOf(step.operationId);
            step.produces.reserve(op->extractions.size());
            for (const auto& extraction : op->extractions) {
                step.produces.push_back(PreviewOutput{
                    .variable =
                        qualifyVariable(resource, QString::fromStdString(extraction.variableName)),
                    .sourcePath = QString::fromStdString(extraction.sourcePath)});
            }
        }

        steps.push_back(std::move(step));
    }

    return steps;
}

QStringList consumersOfVariable(const engine::ResolvedPlan& plan,
                                const QString& producerOperationId,
                                const QString& variable) {
    if (producerOperationId.isEmpty() || variable.isEmpty()) {
        return {};
    }

    // Edges spell the variable "<producing resource>.<stored key>" while
    // extraction events report just the stored key, so normalise instead of
    // trusting the caller's form.
    //
    // The guard is "already starts with this resource's prefix", NOT "contains a
    // dot": a stored key may legitimately contain dots (`employer.org_id`) and
    // still needs the prefix. Ambiguity remains only for a key literally named
    // "<own resource>.<x>", which is vanishingly rare and unresolvable from the
    // name alone.
    const QString resource = resourceOf(producerOperationId);
    const QString qualified =
        (resource.isEmpty() || variable.startsWith(resource + QLatin1Char('.')))
            ? variable
            : resource + QLatin1Char('.') + variable;

    const std::string producer = producerOperationId.toStdString();
    const std::string wanted = qualified.toStdString();

    std::set<std::string> consumers;
    for (const auto& edge : plan.edges) {
        // Explicit depends_on edges carry no variable, so they are not evidence
        // that this particular value was consumed.
        if (edge.producer.value == producer && edge.variable == wanted) {
            consumers.insert(edge.consumer.value);
        }
    }

    QStringList out;
    out.reserve(static_cast<qsizetype>(consumers.size()));
    for (const std::string& consumer : consumers) {
        out.append(QString::fromStdString(consumer));
    }
    return out;
}

}  // namespace reqloom::desktop
