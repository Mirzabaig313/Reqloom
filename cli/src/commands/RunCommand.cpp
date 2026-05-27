// `chainapi run <op>` — load a project, resolve a chain, execute,
// and emit results through the requested renderer (text / json / junit).

#include "RunCommand.h"

#include "../output/JUnitRenderer.h"
#include "../output/JsonRenderer.h"
#include "../output/TextRenderer.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ostream>
#include <print>
#include <string>
#include <utility>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;

namespace chainapi::cli {

namespace {

enum class Format : std::uint8_t { Text, Json, JUnit };

struct RunArgs {
    std::string targetOp;
    fs::path projectPath{fs::current_path()};
    std::string envName;
    std::map<std::string, std::string> overrides;
    Format format{Format::Text};
    fs::path outputPath;  ///< empty = stdout
    bool quiet{false};
};

void printUsage(std::ostream& os) {
    std::println(os,
                 "Usage: chainapi run <resource.operation> [options]\n"
                 "Options:\n"
                 "  --project <path>     Project directory (default: cwd)\n"
                 "  --env <name>         Environment to run against\n"
                 "  --var KEY=VALUE      Override an environment variable (repeatable)\n"
                 "  --format <fmt>       Output format: text (default), json, junit\n"
                 "  --output <file>      Write rendered output to <file> (default: stdout)\n"
                 "  --quiet              Suppress live progress on stdout (failures still go\n"
                 "                       to stderr). Implied by --format json|junit.\n"
                 "Exit codes: 0 success, 1 run/schema failure, 2 invalid arguments.");
}

[[nodiscard]] std::expected<Format, std::string> parseFormat(const std::string& token) {
    if (token == "text") return Format::Text;
    if (token == "json") return Format::Json;
    if (token == "junit") return Format::JUnit;
    return std::unexpected("--format must be one of text, json, junit (got '" + token + "')");
}

[[nodiscard]] std::expected<RunArgs, int> parseArgs(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "chainapi run: missing <operation>");
        printUsage(std::cerr);
        return std::unexpected(2);
    }
    if (args.first() == QStringLiteral("--help") || args.first() == QStringLiteral("-h")) {
        printUsage(std::cout);
        return std::unexpected(0);
    }

    RunArgs out;
    out.targetOp = args.first().toStdString();

    for (int i = 1; i < args.size(); ++i) {
        const auto& flag = args[i];
        if (flag == QStringLiteral("--project") && i + 1 < args.size()) {
            out.projectPath = args[++i].toStdString();
        } else if (flag == QStringLiteral("--env") && i + 1 < args.size()) {
            out.envName = args[++i].toStdString();
        } else if (flag == QStringLiteral("--var") && i + 1 < args.size()) {
            const auto kv = args[++i].toStdString();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) {
                std::println(stderr, "chainapi run: --var requires KEY=VALUE, got '{}'", kv);
                return std::unexpected(2);
            }
            out.overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (flag == QStringLiteral("--format") && i + 1 < args.size()) {
            auto parsed = parseFormat(args[++i].toStdString());
            if (!parsed) {
                std::println(stderr, "chainapi run: {}", parsed.error());
                return std::unexpected(2);
            }
            out.format = *parsed;
        } else if (flag == QStringLiteral("--output") && i + 1 < args.size()) {
            out.outputPath = args[++i].toStdString();
        } else if (flag == QStringLiteral("--quiet")) {
            out.quiet = true;
        } else if (flag == QStringLiteral("--help") || flag == QStringLiteral("-h")) {
            printUsage(std::cout);
            return std::unexpected(0);
        } else {
            std::println(stderr, "chainapi run: unknown argument '{}'", flag.toStdString());
            printUsage(std::cerr);
            return std::unexpected(2);
        }
    }

    // Machine-readable formats imply --quiet because their output contract
    // is a single document on stdout. Mixing live event noise into a JSON
    // document would corrupt the parse on the consumer side.
    if (out.format == Format::Json || out.format == Format::JUnit) {
        out.quiet = true;
    }
    return out;
}

[[nodiscard]] std::ostream& renderTarget(std::ostream& fallback,
                                         const fs::path& path,
                                         std::unique_ptr<std::ofstream>& owned) {
    if (path.empty()) return fallback;
    owned = std::make_unique<std::ofstream>(path);
    if (!*owned) {
        std::println(stderr, "chainapi run: cannot open --output file '{}'", path.string());
        return fallback;  // best-effort fallback; the caller checks owned->good()
    }
    return *owned;
}

}  // namespace

int runCommand(const QStringList& args) {
    auto parsed = parseArgs(args);
    if (!parsed) return parsed.error();
    auto& cfg = *parsed;

    auto yamlPath = cfg.projectPath / "chainapi.yaml";
    if (!fs::exists(yamlPath)) {
        std::println(stderr, "Error: chainapi.yaml not found in {}", cfg.projectPath.string());
        return 1;
    }

    auto projectResult = ce::parseProject(yamlPath);
    if (!projectResult) {
        std::println(stderr,
                     "Schema error [{}]: {}",
                     std::string(ce::toCodeString(projectResult.error().code)),
                     projectResult.error().detail);
        return 1;
    }
    auto& project = *projectResult;

    if (!cfg.overrides.empty()) {
        const auto envKey = cfg.envName.empty() ? project.defaultEnvironment : cfg.envName;
        auto& envVars = project.environments[envKey];
        for (auto& [k, v] : cfg.overrides) {
            envVars[k] = std::move(v);
        }
    }

    // Decide where rendered output goes. `--output` redirects the renderer's
    // sink; live progress (when not quiet) always goes to stdout/stderr so
    // CI tail logs still see the run unfold.
    std::unique_ptr<std::ofstream> ownedSink;
    std::ostream& sink = renderTarget(std::cout, cfg.outputPath, ownedSink);
    if (ownedSink && !ownedSink->good()) {
        return 1;
    }

    // Text mode in quiet mode would be silent; keep the project preamble
    // off when the user wants machine-readable output. The summary always
    // goes to `sink` (stdout or `--output`), live progress to stdout/stderr.
    TextRenderer textRenderer(sink, std::cout, std::cerr, cfg.quiet);
    if (cfg.format == Format::Text && !cfg.quiet) {
        textRenderer.printProjectPreamble(
            project.name, project.actors.size(), project.resources.size());
    }

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    ce::RunOptions options;
    if (!cfg.envName.empty()) options.environment = cfg.envName;

    if (cfg.format == Format::Text) {
        engine.subscribe([&textRenderer](const ce::RunEvent& e) { textRenderer.onEvent(e); });
    }

    const ce::OperationId target{cfg.targetOp};
    auto resolvedEnv = cfg.envName.empty() ? project.defaultEnvironment : cfg.envName;

    auto result = engine.run(project, target, ctx, options);
    if (!result) {
        std::println(stderr,
                     "Engine error [{}]: {}",
                     std::string(ce::toCodeString(result.error().code)),
                     result.error().detail);
        return 1;
    }

    switch (cfg.format) {
        case Format::Text:
            textRenderer.render(target, resolvedEnv, *result);
            break;
        case Format::Json: {
            JsonRenderer renderer(sink);
            renderer.render(target, resolvedEnv, *result);
            break;
        }
        case Format::JUnit: {
            JUnitRenderer renderer(sink);
            renderer.render(target, resolvedEnv, *result);
            break;
        }
    }

    return result->succeeded() ? 0 : 1;
}

}  // namespace chainapi::cli
