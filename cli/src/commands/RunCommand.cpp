#include "RunCommand.h"

#include <chainapi/engine/PublicApi.h>

// Engine infrastructure — we construct the real implementations here.
// This is the CLI's equivalent of desktop/Bootstrapper.cpp.
#include "../../../engine/src/infrastructure/http/CurlHttpClient.h"
#include "../../../engine/src/infrastructure/schema/YamlSchemaParser.h"
#include "../../../engine/src/infrastructure/secrets/KeychainSecretStore.h"
#include "../../../engine/src/infrastructure/storage/SqliteHistoryStore.h"
#include "../../../engine/src/infrastructure/hooks/QuickJsHookRunner.h"

#include <filesystem>
#include <print>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;

namespace chainapi::cli {

int runCommand(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "chainapi run: missing <operation>");
        std::println(stderr, "Usage: chainapi run <resource.operation> [--project <path>] [--env <name>]");
        return 2;
    }

    // Parse arguments.
    auto targetOp = args.first().toStdString();
    fs::path projectPath = fs::current_path();
    std::string envName;

    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--project" && i + 1 < args.size()) {
            projectPath = args[++i].toStdString();
        } else if (args[i] == "--env" && i + 1 < args.size()) {
            envName = args[++i].toStdString();
        }
    }

    // Find chainapi.yaml.
    auto yamlPath = projectPath / "chainapi.yaml";
    if (!fs::exists(yamlPath)) {
        std::println(stderr, "Error: chainapi.yaml not found in {}", projectPath.string());
        return 1;
    }

    // Parse the schema.
    ce::YamlSchemaParser parser;
    auto projectResult = parser.parse(yamlPath);
    if (!projectResult) {
        std::println(stderr, "Schema error [{}]: {}",
                     std::string(ce::toCodeString(projectResult.error().code)),
                     projectResult.error().detail);
        return 1;
    }

    auto& project = *projectResult;
    std::println("Loaded project: {} ({} actors, {} resources)",
                 project.name,
                 project.actors.size(),
                 project.resources.size());

    // Construct engine with real dependencies.
    ce::ExecutionEngine::Dependencies deps;
    deps.http = std::make_unique<ce::CurlHttpClient>();
    deps.schema = std::make_unique<ce::YamlSchemaParser>();
    deps.history = std::make_unique<ce::SqliteHistoryStore>();
    deps.secrets = std::make_unique<ce::KeychainSecretStore>();
    deps.hooks = std::make_unique<ce::QuickJsHookRunner>();

    ce::ExecutionEngine engine(std::move(deps));
    ce::RunContext ctx;
    ce::RunOptions options;
    if (!envName.empty()) options.environment = envName;

    // Subscribe to events for real-time output.
    engine.subscribe([](const ce::RunEvent& event) {
        std::visit([](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, ce::StepStarted>) {
                std::println("  [{}/{}] Running: {} (attempt {})",
                             e.stepIndex + 1, 0, e.op.value, e.attempt);
            } else if constexpr (std::is_same_v<T, ce::StepSkipped>) {
                std::println("  [{}] Skipped: {} (cached)", e.stepIndex + 1, e.op.value);
            } else if constexpr (std::is_same_v<T, ce::StepFailed>) {
                std::println(stderr, "  [{}] FAILED: {} — {}",
                             e.stepIndex + 1, e.op.value, e.detail);
            } else if constexpr (std::is_same_v<T, ce::RunStarted>) {
                std::println("Running: {} (chain of {} steps, env={})",
                             e.target.value, e.chainSize, e.envName);
            } else if constexpr (std::is_same_v<T, ce::RunEnded>) {
                const char* outcome = e.outcome == ce::RunOutcome::Succeeded ? "SUCCEEDED"
                    : e.outcome == ce::RunOutcome::Failed ? "FAILED" : "CANCELLED";
                std::println("\nResult: {}", outcome);
            }
        }, event);
    });

    // Run.
    auto result = engine.run(project, ce::OperationId{targetOp}, ctx, options);
    if (!result) {
        std::println(stderr, "Engine error [{}]: {}",
                     std::string(ce::toCodeString(result.error().code)),
                     result.error().detail);
        return 1;
    }

    // Print step summary.
    std::println("\n--- Chain Summary ---");
    for (const auto& step : result->steps) {
        const char* status = "?";
        switch (step.status) {
            case ce::StepResult::Status::Succeeded: status = "✓"; break;
            case ce::StepResult::Status::Skipped:   status = "⊘"; break;
            case ce::StepResult::Status::Failed:    status = "✗"; break;
            case ce::StepResult::Status::Cancelled: status = "⊘"; break;
            case ce::StepResult::Status::Blocked:   status = "⊘"; break;
            default: break;
        }
        std::println("  {} {} ({}ms)", status, step.op.value, step.elapsed.count());
    }

    return result->succeeded() ? 0 : 1;
}

}  // namespace chainapi::cli
