// `reqloom lint`: validate a project's schema and dry-run every operation's chain.

#include "LintCommand.h"

#include "ArgParser.h"
#include "ExitCodes.h"
#include "PathText.h"

#include "../output/ReportWriter.h"
#include "../output/TextEncoding.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

constexpr std::array kOptions{
    OptionSpec{.name = "--project", .takesValue = true},
    OptionSpec{.name = "--format", .takesValue = true},
};

constexpr std::array<std::string_view, 1> kMachineFormats{"json"};

/// Version of the lint JSON document, written as its `schema_version`.
constexpr int kLintSchemaVersion = 1;

void printUsage(std::FILE* stream) {
    std::print(stream,
               "Usage: reqloom lint [<project-dir>] [options]\n"
               "Validate a project's schema and resolve every operation's chain without\n"
               "sending requests.\n"
               "  <project-dir>, --project <path>\n"
               "                     Project directory (default: cwd); give one or the other\n"
               "  --format <fmt>     Output format: text (default), json. JSON is one document\n"
               "                     on stdout: {{ schema_version, ok, errors: [{{ code, detail,\n"
               "                     operation }}], warnings: [...] }}\n"
               "Exit codes:\n"
               "  0  the project is valid (warnings do not fail it)\n"
               "  1  the project does not validate\n"
               "  2  invalid invocation: unknown option, missing or invalid value, no\n"
               "     reqloom.yaml in the project path\n"
               "  3  the report could not be written to stdout, or an unexpected error\n");
}

struct LintError {
    std::string code;
    std::string detail;
    std::string operation;  ///< empty for a project-level error
};

struct LintResult {
    std::vector<LintError> errors;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::string lintJson(const LintResult& result) {
    std::string out = std::format("{{\n  \"schema_version\": {},\n  \"ok\": {},\n  \"errors\": [",
                                  kLintSchemaVersion,
                                  result.errors.empty() ? "true" : "false");
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        const auto& e = result.errors[i];
        const auto operation =
            e.operation.empty() ? std::string{"null"} : "\"" + jsonEscape(e.operation) + "\"";
        out += std::format("{}\n    {{ \"code\": \"{}\", \"detail\": \"{}\", \"operation\": {} }}",
                           i == 0 ? "" : ",",
                           jsonEscape(e.code),
                           jsonEscape(e.detail),
                           operation);
    }
    out += result.errors.empty() ? "],\n  \"warnings\": [" : "\n  ],\n  \"warnings\": [";
    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        out += std::format("{}\n    \"{}\"", i == 0 ? "" : ",", jsonEscape(result.warnings[i]));
    }
    out += result.warnings.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

/// Prints the JSON document when `json`; returns `exitCode`, or kExitSystem if it was not written.
[[nodiscard]] int finish(bool json, const LintResult& result, int exitCode) {
    if (!json) {
        return exitCode;
    }
    if (const auto written = writeReportStream(stdout, lintJson(result)); !written) {
        std::println(stderr, "reqloom lint: cannot write the report: {}", written.error());
        return kExitSystem;
    }
    return exitCode;
}

[[nodiscard]] int failEarly(bool json,
                            std::string code,
                            const std::string& detail,
                            int exitCode,
                            std::string_view prefix = "Error: ") {
    std::println(stderr, "{}{}", prefix, detail);
    return finish(json, LintResult{{LintError{std::move(code), detail, {}}}, {}}, exitCode);
}

/// Dry-runs every operation so unresolved references and cycles surface without any request.
void dryRunAll(const ce::Project& project, LintResult& result, std::size_t& totalOps) {
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& op : resource.operations | std::views::values) {
            ++totalOps;
            ce::RunContext ctx;
            ce::RunOptions options;
            options.dryRun = true;
            auto run = engine.run(project, op.id, ctx, options);
            if (!run) {
                const auto code = std::string{ce::toCodeString(run.error().code)};
                std::println(stderr, "  ERROR {}: [{}] {}", op.id.value, code, run.error().detail);
                result.errors.push_back(LintError{code, run.error().detail, op.id.value});
            }
        }
    }
}

[[nodiscard]] int lintProject(const fs::path& projectPath, bool json) {
    const auto yamlPath = projectPath / "reqloom.yaml";
    std::error_code ec;
    if (!fs::exists(yamlPath, ec)) {
        return failEarly(json,
                         kCodeProjectNotFound,
                         "reqloom.yaml not found in " + pathToUtf8(projectPath),
                         kExitUsage);
    }
    LintResult result;
    auto projectResult = ce::parseProject(yamlPath);
    if (!projectResult) {
        const auto code = std::string{ce::toCodeString(projectResult.error().code)};
        std::println(stderr, "LINT FAIL [{}]: {}", code, projectResult.error().detail);
        result.errors.push_back(LintError{code, projectResult.error().detail, {}});
        return finish(json, result, kExitFailed);
    }
    const auto& project = *projectResult;
    result.warnings = project.loadWarnings;
    if (project.id.empty() && !ce::collectSecretReferences(project).empty()) {
        result.warnings.push_back(std::format(
            "project uses secrets but has no id; runs will fail. Add `id: {}` to reqloom.yaml.",
            ce::generateProjectId()));
    }
    // Load warnings (unknown keys, imports that matched nothing) never fail the lint.
    for (const auto& warning : result.warnings) {
        std::println(stderr, "  WARNING: {}", warning);
    }
    std::size_t totalOps = 0;
    dryRunAll(project, result, totalOps);
    if (!result.errors.empty()) {
        std::println(stderr, "\nLINT FAILED — {} error(s).", result.errors.size());
        return finish(json, result, kExitFailed);
    }
    if (!json) {
        std::println("LINT OK — {} actors, {} resources, {} operations. No errors.",
                     project.actors.size(),
                     project.resources.size(),
                     totalOps);
    }
    return finish(json, result, kExitSuccess);
}

/// The project directory and whether the format is JSON, or why the arguments are invalid.
[[nodiscard]] std::expected<std::pair<fs::path, bool>, std::string> toLintArgs(
    const ParsedArguments& parsed) {
    if (parsed.positionals.size() > 1) {
        return std::unexpected(std::format("unexpected argument '{}'", parsed.positionals[1]));
    }
    const auto flag = parsed.value("--project");
    if (flag && !parsed.positionals.empty()) {
        return std::unexpected(
            std::string{"give the project directory either as an argument or with --project"});
    }
    const auto format = parsed.value("--format").value_or("text");
    if (format != "text" && format != "json") {
        return std::unexpected(
            std::format("--format must be one of text, json (got '{}')", format));
    }
    fs::path project = fs::current_path();
    if (flag) {
        project = pathFromUtf8(*flag);
    } else if (!parsed.positionals.empty()) {
        project = pathFromUtf8(parsed.positionals.front());
    }
    return std::pair{project, format == "json"};
}

}  // namespace

int lintCommand(const QStringList& args) {
    const auto parsed = parseArguments(args, kOptions);
    if (parsed && parsed->help) {
        printUsage(stdout);
        return kExitSuccess;
    }
    const auto lintArgs = parsed ? toLintArgs(*parsed)
                                 : std::expected<std::pair<fs::path, bool>, std::string>{
                                       std::unexpect, parsed.error()};
    if (!lintArgs) {
        const bool json = peekFormat(args, kMachineFormats).has_value();
        const int code =
            failEarly(json, kCodeUsage, lintArgs.error(), kExitUsage, "reqloom lint: ");
        std::println(stderr, "Run 'reqloom lint --help' for usage.");
        return code;
    }
    const auto& [projectPath, json] = *lintArgs;
    try {
        return lintProject(projectPath, json);
    } catch (const std::exception& ex) {
        return failEarly(
            json, kCodeInternal, std::format("unexpected failure: {}", ex.what()), kExitSystem);
    }
}

}  // namespace reqloom::cli
