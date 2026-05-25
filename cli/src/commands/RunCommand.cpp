#include "RunCommand.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <filesystem>
#include <print>
#include <utility>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;

namespace chainapi::cli {

namespace {

void renderEvent(const ce::RunEvent& event) {
    std::visit([](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ce::RunStarted>) {
            std::println("Running: {} (chain of {} steps, env={})",
                         e.target.value, e.chainSize, e.envName);
        } else if constexpr (std::is_same_v<T, ce::StepStarted>) {
            std::println("  [{}] Running: {} (attempt {})",
                         e.stepIndex + 1, e.op.value, e.attempt);
        } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
            std::println("  [{}] Skipped: {} (cached)", e.stepIndex + 1, e.op.value);
        } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
            std::println(stderr, "  [{}] FAILED: {} — {}",
                         e.stepIndex + 1, e.op.value, e.detail);
        } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
            const char* outcome =
                  e.outcome == ce::RunOutcome::Succeeded ? "SUCCEEDED"
                : e.outcome == ce::RunOutcome::Failed    ? "FAILED"
                                                          : "CANCELLED";
            std::println("\nResult: {}", outcome);
        }
    }, event);
}

[[nodiscard]] const char* statusGlyph(ce::StepResult::Status status) noexcept {
    switch (status) {
        case ce::StepResult::Status::Succeeded: return "OK";
        case ce::StepResult::Status::Skipped:   return "SK";
        case ce::StepResult::Status::Failed:    return "FAIL";
        case ce::StepResult::Status::Cancelled: return "CANCEL";
        case ce::StepResult::Status::Blocked:   return "BLOCK";
        default:                                return "?";
    }
}

[[nodiscard]] std::string errorCodeName(const ce::StepResult& step) {
    if (!step.error) return "—";
    return std::string(ce::toCodeString(*step.error));
}

}  // namespace

int runCommand(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "chainapi run: missing <operation>");
        std::println(stderr,
                     "Usage: chainapi run <resource.operation> "
                     "[--project <path>] [--env <name>]");
        return 2;
    }

    auto targetOp = args.first().toStdString();
    fs::path projectPath = fs::current_path();
    std::string envName;

    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--project") && i + 1 < args.size()) {
            projectPath = args[++i].toStdString();
        } else if (args[i] == QStringLiteral("--env") && i + 1 < args.size()) {
            envName = args[++i].toStdString();
        }
    }

    auto yamlPath = projectPath / "chainapi.yaml";
    if (!fs::exists(yamlPath)) {
        std::println(stderr, "Error: chainapi.yaml not found in {}",
                     projectPath.string());
        return 1;
    }

    auto projectResult = ce::parseProject(yamlPath);
    if (!projectResult) {
        std::println(stderr, "Schema error [{}]: {}",
                     std::string(ce::toCodeString(projectResult.error().code)),
                     projectResult.error().detail);
        return 1;
    }

    auto& project = *projectResult;
    std::println("Loaded project: {} ({} actors, {} resources)",
                 project.name, project.actors.size(), project.resources.size());

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    ce::RunOptions options;
    if (!envName.empty()) options.environment = std::move(envName);

    engine.subscribe(renderEvent);

    auto result = engine.run(project, ce::OperationId{targetOp}, ctx, options);
    if (!result) {
        std::println(stderr, "Engine error [{}]: {}",
                     std::string(ce::toCodeString(result.error().code)),
                     result.error().detail);
        return 1;
    }

    std::println("\n--- Chain Summary ---");
    for (const auto& step : result->steps) {
        std::println("  {:<6} {} ({}ms) err={}",
                     statusGlyph(step.status),
                     step.op.value,
                     step.elapsed.count(),
                     errorCodeName(step));
        if (!step.detail.empty()) {
            std::println("         {}", step.detail);
        }
    }

    return result->succeeded() ? 0 : 1;
}

}  // namespace chainapi::cli
