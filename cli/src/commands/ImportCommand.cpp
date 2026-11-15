// `reqloom import <spec>`: import an API definition or client export into a Reqloom project.

#include "ImportCommand.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>
#include <reqloom/engine/SecretStore.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
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
    bool noSecrets{false};
};

void printUsage(std::FILE* stream) {
    std::println(stream,
                 "Usage: reqloom import <spec> [options]\n"
                 "Import an API definition into a project. Supported formats,\n"
                 "auto-detected: OpenAPI 3.x and Swagger 2.0 (YAML/JSON), Postman v2.1,\n"
                 "Insomnia v4, Thunder Client, Hoppscotch, HTTPie, Apidog, REST Client\n"
                 "(.http/.rest), and Bruno (a collection directory, its bruno.json, or a\n"
                 ".bru file).\n"
                 "Options:\n"
                 "  --out <dir>           Directory to write the project into (default: cwd)\n"
                 "  --project-root <dir>  Containment root the spec must resolve under\n"
                 "                        (default: the spec's parent directory)\n"
                 "  --force               Allow writing into a <dir> that already has a\n"
                 "                        reqloom.yaml: replaces reqloom.yaml and any\n"
                 "                        actors/, resources/, environments/ file with the\n"
                 "                        same name as an imported one. Never deletes\n"
                 "                        other files.\n"
                 "  --no-secrets          Leave credentials found in the input (auth headers,\n"
                 "                        variables the source marks secret) in the YAML in\n"
                 "                        plain text instead of moving them into the\n"
                 "                        project's secrets in the OS keychain.\n"
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
        } else if (flag == QStringLiteral("--no-secrets")) {
            out.noSecrets = true;
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

[[nodiscard]] std::size_t operationCount(const ce::Project& project) {
    std::size_t total{0};
    for (const auto& resource : project.resources | std::views::values) {
        total += resource.operations.size();
    }
    return total;
}

/// Names only, never values.
void printCredentialSummary(const ce::CredentialMoveReport& report) {
    if (!report.moved.empty()) {
        std::string names;
        for (const auto& name : report.moved) {
            names += (names.empty() ? "" : ", ") + name;
        }
        std::println("Moved {} credential{} into the project's secrets: {}",
                     report.moved.size(),
                     report.moved.size() == 1 ? "" : "s",
                     names);
    }
    if (!report.kept.empty()) {
        std::println(stderr,
                     "\nLeft {} credential{} in the YAML in plain text:",
                     report.kept.size(),
                     report.kept.size() == 1 ? "" : "s");
        for (const auto& line : report.warnings()) {
            std::println(stderr, "  - {}", line);
        }
    }
}

}  // namespace

int importCommand(const QStringList& args) {
    const auto store = ce::makeKeychainSecretStore();
    const auto trust = ce::makeFileProjectTrustStore();
    return importCommand(args, store.get(), trust.get());
}

int importCommand(const QStringList& args, ce::SecretStore* store, ce::ProjectTrustStore* trust) {
    auto parsed = parseArgs(args);
    if (!parsed) {
        return parsed.error();
    }
    auto& cfg = *parsed;

    if (!fs::exists(cfg.spec)) {
        std::println(stderr, "reqloom import: spec not found: {}", cfg.spec.string());
        return 1;
    }

    // Default the containment root to the spec's parent; `--project-root` tightens it.
    const fs::path projectRoot = cfg.projectRoot.empty() ? cfg.spec.parent_path() : cfg.projectRoot;

    auto imported = ce::importAny(cfg.spec, projectRoot);
    if (!imported) {
        std::println(stderr,
                     "Import error [{}]: {}",
                     ce::toCodeString(imported.error().code),
                     imported.error().detail);
        return 1;
    }

    // Never prunes other actors/resources/environments in <dir>: they may be hand-written.
    auto written =
        ce::writeImportedProject(cfg.outDir,
                                 imported->project,
                                 imported->credentials,
                                 store,
                                 trust,
                                 {.overwrite = cfg.force, .moveCredentials = !cfg.noSecrets});
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
                 written->reqloomYaml.string());
    printCredentialSummary(written->credentials);

    // Warnings go to stderr so the stdout success line stays machine-friendly.
    if (!imported->warnings.empty()) {
        std::println(stderr, "\nReview notes:\n{}", imported->warnings);
    }

    return 0;
}

}  // namespace reqloom::cli
