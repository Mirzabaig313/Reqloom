// ExecutionPreview — see header.
#include "ExecutionPreview.h"

#include "views/Formatting.h"

#include <algorithm>
#include <map>
#include <set>

namespace reqloom::desktop {

namespace {

/// Locate an operation by its "<resource>.<op_name>" id, split at the first dot as the engine
/// does (an operation name may contain dots). Returns nullptr when either half is unknown.
[[nodiscard]] const engine::Operation* findOperation(const engine::Project& project,
                                                     const std::string& id) {
    const std::size_t dot = id.find('.');
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

/// Resource half of an operation id ("orders.create" → "orders"), also the namespace of its
/// extracted variables. Empty if none.
[[nodiscard]] QString resourceOf(const QString& operationId) {
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    return dot > 0 ? operationId.left(dot) : QString{};
}

/// Namespace an extraction name as a template must reference it: `<producing resource>.<stored
/// key>`. The prefix applies even to dotted keys: `substituteRef` splits at the FIRST dot only, so
/// `employer.org_id` stored in `admin_organization` is referenced as
/// `{{admin_organization.employer.org_id}}`.
[[nodiscard]] QString qualifyVariable(const QString& resource, const QString& variableName) {
    if (resource.isEmpty()) {
        return variableName;
    }
    return resource + QLatin1Char('.') + variableName;
}

/// Producer ids per consumer, de-duplicated: an explicit depends_on and an implicit `{{var}}`
/// reference are one dependency to a reader.
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
        step.isTarget = (i + 1 == plan.order.size());

        if (const auto producerIt = producers.find(id); producerIt != producers.end()) {
            step.dependsOn.reserve(static_cast<qsizetype>(producerIt->second.size()));
            for (const std::string& producer : producerIt->second) {
                step.dependsOn.append(QString::fromStdString(producer));
            }
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

    // Edges spell the variable "<producing resource>.<stored key>" while extraction events report
    // the stored key, so normalise. Guard on "already starts with this resource's prefix", not
    // "contains a dot": stored keys may contain dots.
    const QString resource = resourceOf(producerOperationId);
    const QString qualified =
        (resource.isEmpty() || variable.startsWith(resource + QLatin1Char('.')))
            ? variable
            : resource + QLatin1Char('.') + variable;

    const std::string producer = producerOperationId.toStdString();
    const std::string wanted = qualified.toStdString();

    std::set<std::string> consumers;
    for (const auto& edge : plan.edges) {
        // Explicit depends_on edges carry no variable, so they aren't evidence this value was
        // consumed.
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
