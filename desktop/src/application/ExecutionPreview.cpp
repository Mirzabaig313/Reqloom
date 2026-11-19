// ExecutionPreview — see header. A Qt-typed adapter over the engine's `buildChainPreview` and
// `consumersOfVariable` (ProjectInsight.h), which the CLI's `plan` and `describe` share.
#include "ExecutionPreview.h"

#include <reqloom/engine/ProjectInsight.h>

namespace reqloom::desktop {

namespace {

[[nodiscard]] QStringList toQStringList(const std::vector<std::string>& items) {
    QStringList out;
    out.reserve(static_cast<qsizetype>(items.size()));
    for (const auto& item : items) {
        out.append(QString::fromStdString(item));
    }
    return out;
}

}  // namespace

std::vector<PreviewStep> buildExecutionPreview(const engine::ResolvedPlan& plan,
                                               const engine::Project& project) {
    const auto chain = engine::buildChainPreview(plan, project);
    std::vector<PreviewStep> steps;
    steps.reserve(chain.size());
    for (const auto& c : chain) {
        PreviewStep step;
        step.number = c.number;
        step.operationId = QString::fromStdString(c.operationId);
        step.method = QString::fromStdString(c.method);
        step.pathTemplate = QString::fromStdString(c.pathTemplate);
        step.actor = QString::fromStdString(c.actor);
        step.isTarget = c.isTarget;
        step.dependsOn = toQStringList(c.dependsOn);
        step.produces.reserve(c.produces.size());
        for (const auto& output : c.produces) {
            step.produces.push_back(
                PreviewOutput{.variable = QString::fromStdString(output.variable),
                              .sourcePath = QString::fromStdString(output.sourcePath)});
        }
        step.expectStatus.reserve(static_cast<qsizetype>(c.expectStatus.size()));
        for (const int status : c.expectStatus) {
            step.expectStatus.append(status);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

QStringList consumersOfVariable(const engine::ResolvedPlan& plan,
                                const QString& producerOperationId,
                                const QString& variable) {
    return toQStringList(engine::consumersOfVariable(
        plan, producerOperationId.toStdString(), variable.toStdString()));
}

}  // namespace reqloom::desktop
