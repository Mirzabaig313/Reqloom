// `reqloom import <spec>`: import an API definition or client export into a Reqloom project.

#include "ImportCommand.h"

#include "ArgParser.h"
#include "ExitCodes.h"
#include "PathText.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>
#include <reqloom/engine/SecretStore.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <ranges>
#include <string>
#include <system_error>
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

constexpr std::array kOptions{
    OptionSpec{.name = "--out", .takesValue = true},
    OptionSpec{.name = "--project-root", .takesValue = true},
    OptionSpec{.name = "--force"},
    OptionSpec{.name = "--no-secrets"},
};

void printUsage(std::FILE* stream) {
    std::println(stream,
                 "Usage: reqloom import <spec> [options]\n"
                 "Import an API definition into a project. Supported formats,\n"
                 "auto-detected: OpenAPI 3.x and Swagger 2.0 (YAML/JSON), Postman v2.1,\n"
                 "Insomnia v4, Thunder Client, Hoppscotch, HTTPie, Apidog, REST Client\n"
                 "(.http/.rest), and Bruno (a collection directory, its bruno.json, or a\n"
                 ".bru file).\n"
                 "Options may come before or after <spec>; --name=value also works, and\n"
                 "-- ends the options.\n"
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
                 "Exit codes:\n"
                 "  0  imported\n"
                 "  1  the input could not be converted, or <dir> already has a reqloom.yaml\n"
                 "     and --force was not given\n"
                 "  2  invalid invocation: unknown option, missing value, <spec> not found\n"
                 "  3  the project could not be written to <dir>, or an unexpected error");
}

[[nodiscard]] std::expected<ImportArgs, int> usageError(const std::string& message) {
    std::println(stderr, "reqloom import: {}", message);
    std::println(stderr, "Run 'reqloom import --help' for usage.");
    return std::unexpected(kExitUsage);
}

[[nodiscard]] std::expected<ImportArgs, int> parseArgs(const QStringList& args) {
    const auto parsed = parseArguments(args, kOptions);
    if (!parsed) {
        return usageError(parsed.error());
    }
    if (parsed->help) {
        printUsage(stdout);
        return std::unexpected(kExitSuccess);
    }
    if (parsed->positionals.empty()) {
        return usageError("missing <spec>");
    }
    if (parsed->positionals.size() > 1) {
        return usageError(std::format("unexpected argument '{}'", parsed->positionals[1]));
    }
    ImportArgs out;
    out.spec = pathFromUtf8(parsed->positionals.front());
    if (const auto outDir = parsed->value("--out")) {
        out.outDir = pathFromUtf8(*outDir);
    }
    if (const auto root = parsed->value("--project-root")) {
        out.projectRoot = pathFromUtf8(*root);
    }
    out.force = parsed->has("--force");
    out.noSecrets = parsed->has("--no-secrets");
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

    std::error_code ec;
    if (!fs::exists(cfg.spec, ec)) {
        std::println(stderr, "reqloom import: spec not found: {}", pathToUtf8(cfg.spec));
        return kExitUsage;
    }

    // Default the containment root to the spec's parent; `--project-root` tightens it.
    const fs::path projectRoot = cfg.projectRoot.empty() ? cfg.spec.parent_path() : cfg.projectRoot;

    auto imported = ce::importAny(cfg.spec, projectRoot);
    if (!imported) {
        std::println(stderr,
                     "Import error [{}]: {}",
                     ce::toCodeString(imported.error().code),
                     imported.error().detail);
        return kExitFailed;
    }
    if (!cfg.force && fs::exists(cfg.outDir / "reqloom.yaml", ec)) {
        std::println(stderr,
                     "reqloom import: {} already has a reqloom.yaml (pass --force to overwrite it)",
                     pathToUtf8(cfg.outDir));
        return kExitFailed;
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
        return kExitSystem;
    }

    std::println("Imported {} resources, {} operations into {}",
                 imported->project.resources.size(),
                 operationCount(imported->project),
                 pathToUtf8(written->reqloomYaml));
    printCredentialSummary(written->credentials);

    // Warnings go to stderr so the stdout success line stays machine-friendly.
    if (!imported->warnings.empty()) {
        std::println(stderr, "\nReview notes:\n{}", imported->warnings);
    }

    return kExitSuccess;
}

}  // namespace reqloom::cli
