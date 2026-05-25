#include "LintCommand.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <filesystem>
#include <print>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;

namespace chainapi::cli {

int lintCommand(const QStringList& args) {
    fs::path projectPath = fs::current_path();
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--project") && i + 1 < args.size()) {
            projectPath = args[++i].toStdString();
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
        std::println(stderr, "LINT FAIL [{}]: {}",
                     std::string(ce::toCodeString(projectResult.error().code)),
                     projectResult.error().detail);
        return 1;
    }

    // Build an engine just to access the resolver via run-with-dry-run.
    // For pure schema validation we'd want a standalone validator helper —
    // adding that to Factories.h is a follow-up. For now, run dry-run for
    // every operation and treat resolution errors as lint failures.
    auto& project = *projectResult;
    int errors = 0;
    std::size_t totalOps = 0;

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            ++totalOps;
            ce::RunContext ctx;
            ce::RunOptions options;
            options.dryRun = true;
            auto result = engine.run(project, op.id, ctx, options);
            if (!result) {
                std::println(stderr, "  ERROR {}: [{}] {}",
                             op.id.value,
                             std::string(ce::toCodeString(result.error().code)),
                             result.error().detail);
                ++errors;
            }
        }
    }

    if (errors == 0) {
        std::println("LINT OK — {} actors, {} resources, {} operations. No errors.",
                     project.actors.size(), project.resources.size(), totalOps);
        return 0;
    }

    std::println(stderr, "\nLINT FAILED — {} error(s).", errors);
    return 1;
}

}  // namespace chainapi::cli
