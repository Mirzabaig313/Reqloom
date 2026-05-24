#include "LintCommand.h"

#include <chainapi/engine/PublicApi.h>
#include "../../../engine/src/infrastructure/schema/YamlSchemaParser.h"
#include "../../../engine/src/domain/DependencyResolver.h"

#include <filesystem>
#include <print>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;

namespace chainapi::cli {

int lintCommand(const QStringList& args) {
    fs::path projectPath = fs::current_path();

    // Optional --project flag.
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--project" && i + 1 < args.size()) {
            projectPath = args[++i].toStdString();
        }
    }

    auto yamlPath = projectPath / "chainapi.yaml";
    if (!fs::exists(yamlPath)) {
        std::println(stderr, "Error: chainapi.yaml not found in {}", projectPath.string());
        return 1;
    }

    // Parse.
    ce::YamlSchemaParser parser;
    auto projectResult = parser.parse(yamlPath);
    if (!projectResult) {
        std::println(stderr, "LINT FAIL [{}]: {}",
                     std::string(ce::toCodeString(projectResult.error().code)),
                     projectResult.error().detail);
        return 1;
    }

    auto& project = *projectResult;
    std::println("Parsed: {} ({} actors, {} resources)",
                 project.name, project.actors.size(), project.resources.size());

    // Validate all operations can resolve their dependency chains.
    ce::DependencyResolver resolver;
    int errors = 0;

    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            auto result = resolver.resolve(project, op.id);
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
        // Count total operations.
        std::size_t totalOps = 0;
        for (const auto& [_, r] : project.resources) {
            totalOps += r.operations.size();
        }
        std::println("LINT OK — {} actors, {} resources, {} operations. No errors.",
                     project.actors.size(), project.resources.size(), totalOps);
        return 0;
    }

    std::println(stderr, "\nLINT FAILED — {} error(s).", errors);
    return 1;
}

}  // namespace chainapi::cli
