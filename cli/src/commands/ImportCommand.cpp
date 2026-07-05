// `reqloom import <spec>` — import an OpenAPI 3.x document into a Reqloom
// project skeleton (reqloom.yaml + per-entity sub-files) via the engine's
// importFromOpenApi() + writeProject().

#include "ImportCommand.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <print>
#include <ranges>
#include <string>
#include <utility>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

struct ImportArgs {
    fs::path spec;
    fs::path outDir{fs::current_path()};
    fs::path projectRoot;  ///< empty → default to the spec's parent directory.
    bool force{false};
};

void printUsage(std::FILE* stream) {
    std::println(stream,
                 "Usage: reqloom import <spec> [options]\n"
                 "Import an API definition into a project. Supported formats,\n"
                 "auto-detected: OpenAPI 3.x (YAML/JSON), Postman v2.1, Insomnia v4,\n"
                 "Thunder Client, Hoppscotch, REST Client (.http/.rest), and Bruno\n"
                 "(a collection directory, its bruno.json, or a .bru file).\n"
                 "Options:\n"
                 "  --out <dir>           Directory to write the project into (default: cwd)\n"
                 "  --project-root <dir>  Containment root the spec must resolve under\n"
                 "                        (default: the spec's parent directory)\n"
                 "  --force               Overwrite an existing reqloom.yaml in <dir>\n"
                 "Exit codes: 0 success, 1 import/write failure, 2 invalid arguments.");
}

[[nodiscard]] std::expected<ImportArgs, int> parseArgs(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "reqloom import: missing <spec>");
        printUsage(stderr);
        return std::unexpected(2);
    }
    if (args.first() == QStringLiteral("--help") || args.first() == QStringLiteral("-h")) {
        printUsage(stdout);
        return std::unexpected(0);
    }

    ImportArgs out;
    out.spec = args.first().toStdString();

    for (int i = 1; i < args.size(); ++i) {
        const auto& flag = args[i];
        if (flag == QStringLiteral("--out") && i + 1 < args.size()) {
            out.outDir = args[++i].toStdString();
        } else if (flag == QStringLiteral("--project-root") && i + 1 < args.size()) {
            out.projectRoot = args[++i].toStdString();
        } else if (flag == QStringLiteral("--force")) {
            out.force = true;
        } else if (flag == QStringLiteral("--help") || flag == QStringLiteral("-h")) {
            printUsage(stdout);
            return std::unexpected(0);
        } else if (flag == QStringLiteral("--out") || flag == QStringLiteral("--project-root")) {
            std::println(stderr, "reqloom import: missing value for '{}'", flag.toStdString());
            return std::unexpected(2);
        } else {
            std::println(stderr, "reqloom import: unknown argument '{}'", flag.toStdString());
            printUsage(stderr);
            return std::unexpected(2);
        }
    }
    return out;
}

/// Count operations across every resource for the success summary.
[[nodiscard]] std::size_t operationCount(const ce::Project& project) {
    std::size_t total{0};
    for (const auto& resource : project.resources | std::views::values) {
        total += resource.operations.size();
    }
    return total;
}

}  // namespace

int importCommand(const QStringList& args) {
    auto parsed = parseArgs(args);
    if (!parsed) {
        return parsed.error();
    }
    auto& cfg = *parsed;

    if (!fs::exists(cfg.spec)) {
        std::println(stderr, "reqloom import: spec not found: {}", cfg.spec.string());
        return 1;
    }

    // Default the containment root to the spec's parent so an explicitly named
    // file always resolves inside it; `--project-root` tightens this when the
    // caller wants to confine the import to a known project tree.
    const fs::path projectRoot = cfg.projectRoot.empty() ? cfg.spec.parent_path() : cfg.projectRoot;

    auto imported = ce::importAny(cfg.spec, projectRoot);
    if (!imported) {
        std::println(stderr,
                     "Import error [{}]: {}",
                     ce::toCodeString(imported.error().code),
                     imported.error().detail);
        return 1;
    }

    auto written = ce::writeProject(cfg.outDir, imported->project, cfg.force);
    if (!written) {
        std::println(stderr,
                     "Write error [{}]: {}",
                     ce::toCodeString(written.error().code),
                     written.error().detail);
        if (!cfg.force) {
            std::println(stderr, "  (pass --force to overwrite an existing reqloom.yaml)");
        }
        return 1;
    }

    std::println("Imported {} resources, {} operations into {}",
                 imported->project.resources.size(),
                 operationCount(imported->project),
                 written->string());

    // Per-operation notes (path params to wire to upstream extractions, etc.)
    // go to stderr so the success line on stdout stays machine-friendly.
    if (!imported->warnings.empty()) {
        std::println(stderr, "\nReview notes:\n{}", imported->warnings);
    }

    return 0;
}

}  // namespace reqloom::cli
