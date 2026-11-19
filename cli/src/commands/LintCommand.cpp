// `reqloom lint`: validate a project's schema and dry-run every operation's chain.

#include "LintCommand.h"

#include "ArgParser.h"
#include "ExitCodes.h"
#include "PathText.h"

#include "../output/Contract.h"
#include "../output/JsonWriter.h"
#include "../output/ReportWriter.h"
#include "../output/TerminalText.h"
#include "../output/TextEncoding.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectInsight.h>
#include <reqloom/engine/PublicApi.h>
#include <reqloom/engine/SecretStore.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
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

void printUsage(std::FILE* stream) {
    std::print(stream,
               "Usage: reqloom lint [<project-dir>] [options]\n"
               "Validate a project's schema and resolve every operation's chain without\n"
               "sending requests.\n"
               "  <project-dir>, --project <path>\n"
               "                     Project directory (default: cwd); give one or the other\n"
               "  --format <fmt>     Output format: text (default), json. JSON is one document\n"
               "                     on stdout: {{ schema_version, contract_version, command,\n"
               "                     ok, errors: [{{ code, detail, operation, file, line,\n"
               "                     column, reference, fix, next_action }}], warnings:\n"
               "                     [\"...\"], warning_details: [{{ message, file, line,\n"
               "                     column, reference, fix }}] }}. file is relative to the\n"
               "                     project; line and column are 1-based, null when unknown\n"
               "Exit codes:\n"
               "  0  the project is valid (warnings do not fail it)\n"
               "  1  the project does not validate\n"
               "  2  invalid invocation: unknown option, missing or invalid value, no\n"
               "     reqloom.yaml in the project path\n"
               "  3  the report could not be written to stdout, or an unexpected error\n");
}

/// Where a finding is, the offending reference and what would satisfy it (FR-15.5); empty or 0
/// when not known.
struct Position {
    std::string file;  ///< As the parser opened it; reported relative to the project.
    int line{0};
    int column{0};
    std::string reference;
    std::string fix;
};

struct LintError {
    std::string code;
    std::string detail;
    std::string operation;  ///< empty for a project-level error
    Position at{};
};

struct LintWarning {
    std::string message;
    Position at{};
};

struct LintResult {
    std::vector<LintError> errors;
    std::vector<LintWarning> warnings;
};

[[nodiscard]] Position positionOf(const ce::ReqloomError& error) {
    return Position{error.file, error.line, error.column, error.reference, error.fix};
}

/// `file` relative to the project folder when it is inside it (`resources/order.yaml`).
[[nodiscard]] std::string relativeFile(const fs::path& projectPath, const std::string& file) {
    if (file.empty()) {
        return file;
    }
    std::error_code ec;
    const auto relative = fs::relative(pathFromUtf8(file), projectPath, ec);
    if (ec || relative.empty() || *relative.begin() == "..") {
        return file;
    }
    return relative.generic_string();
}

void writePosition(JsonWriter& json, const fs::path& projectPath, const Position& at) {
    json.fieldOrNull("file", relativeFile(projectPath, at.file));
    json.fieldOrNull("line", at.line > 0 ? std::optional<std::int64_t>{at.line} : std::nullopt);
    json.fieldOrNull("column",
                     at.column > 0 ? std::optional<std::int64_t>{at.column} : std::nullopt);
    json.fieldOrNull("reference", at.reference);
    json.fieldOrNull("fix", at.fix);
}

/// One line of advice for an error, from Reqloom's own data (the code and whether a position is
/// known), never from project text.
[[nodiscard]] std::string lintNextAction(const LintError& e) {
    if (e.code == kCodeUsage) {
        return "Fix the command line; `reqloom lint --help` lists the options.";
    }
    if (e.code == kCodeProjectNotFound) {
        return "Pass the folder that holds reqloom.yaml, as an argument or with --project.";
    }
    if (e.at.line > 0) {
        return "Edit the file at the line given (file, line), doing what fix says, then run "
               "`reqloom lint` again.";
    }
    return "Fix what detail names in the project YAML, then run `reqloom lint` again.";
}

[[nodiscard]] std::string lintJson(const fs::path& projectPath, const LintResult& result) {
    JsonWriter json;
    json.beginObject()
        .field("schema_version", kSchemaVersion)
        .field("contract_version", kContractVersion)
        .field("command", "lint")
        .field("ok", result.errors.empty());
    json.key("errors").beginArray();
    for (const auto& e : result.errors) {
        json.beginObject().field("code", e.code).field("detail", e.detail);
        json.fieldOrNull("operation", e.operation);
        writePosition(json, projectPath, e.at);
        json.field("next_action", lintNextAction(e));
        json.endObject();
    }
    json.endArray();
    json.key("warnings").beginArray();
    for (const auto& w : result.warnings) {
        json.string(w.message);
    }
    json.endArray();
    json.key("warning_details").beginArray();
    for (const auto& w : result.warnings) {
        json.beginObject().field("message", w.message);
        writePosition(json, projectPath, w.at);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    return json.str();
}

/// The error on stderr, with where it is, the reference and the fix when known.
/// `headline` is the first line, already formatted (`LINT FAIL [E_…]: detail`).
void printError(const std::string& headline, const fs::path& projectPath, const LintError& e) {
    std::println(stderr, "{}", headline);
    if (e.at.line > 0) {
        std::println(stderr,
                     "    at {}:{}{}",
                     terminalSafe(relativeFile(projectPath, e.at.file)),
                     e.at.line,
                     e.at.column > 0 ? std::format(":{}", e.at.column) : std::string{});
    }
    if (!e.at.reference.empty()) {
        std::println(stderr, "    reference: {}", terminalSafe(e.at.reference));
    }
    if (!e.at.fix.empty()) {
        std::println(stderr, "    fix: {}", terminalSafe(e.at.fix));
    }
}

/// Prints the JSON document when `json`; returns `exitCode`, or kExitSystem if it was not written.
[[nodiscard]] int finish(bool json,
                         const LintResult& result,
                         int exitCode,
                         const fs::path& projectPath = {}) {
    if (!json) {
        return exitCode;
    }
    if (const auto written = writeReportStream(stdout, lintJson(projectPath, result)); !written) {
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
    std::println(stderr, "{}{}", prefix, terminalSafe(detail));
    return finish(json, LintResult{{LintError{std::move(code), detail, {}, {}}}, {}}, exitCode);
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
                LintError error{std::string{ce::toCodeString(run.error().code)},
                                run.error().detail,
                                op.id.value,
                                positionOf(run.error())};
                // A chain error without a position points at the operation whose chain failed.
                if (error.at.file.empty()) {
                    error.at.file = op.source.file;
                    error.at.line = op.source.line;
                    error.at.column = op.source.column;
                }
                printError(std::format("  ERROR {}: [{}] {}",
                                       terminalSafe(op.id.value),
                                       error.code,
                                       terminalSafe(error.detail)),
                           project.rootDir,
                           error);
                result.errors.push_back(std::move(error));
            }
        }
    }
}

/// `name` with every character no environment variable name may hold replaced by '_'.
[[nodiscard]] std::string suggestedSecretName(std::string_view name) {
    std::string out{};
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out.push_back(ok ? c : '_');
    }
    return out;
}

/// A name no environment variable can carry still works from the keychain, so this warns rather
/// than fails: on CI or in an agent sandbox (no keychain) the secret would have no source at all.
void appendSecretNameWarnings(const ce::Project& project, std::vector<LintWarning>& warnings) {
    std::map<std::string, std::string> firstByVariable{};
    for (const auto& name : ce::collectSecretReferences(project)) {
        if (const auto variable = ce::secretEnvironmentVariable(name)) {
            const auto [it, inserted] = firstByVariable.try_emplace(*variable, name);
            if (!inserted) {
                warnings.push_back(LintWarning{std::format(
                    "secrets '{}' and '{}' both map to the environment variable {}; a CLI run "
                    "with it set fails rather than give both the same value. Rename one.",
                    it->second,
                    name,
                    *variable)});
            }
        } else {
            warnings.push_back(LintWarning{std::format(
                "secret '{}' cannot be supplied through an environment variable: only names made "
                "of letters, digits and '_' map to {}<NAME>. Without a keychain (CI, agent "
                "sandboxes) it has no source; rename it, for example to '{}'.",
                name,
                ce::kSecretEnvironmentPrefix,
                suggestedSecretName(name))});
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
        const auto& failure = projectResult.error();
        LintError error{
            std::string{ce::toCodeString(failure.code)}, failure.detail, {}, positionOf(failure)};
        printError(std::format("LINT FAIL [{}]: {}", error.code, terminalSafe(error.detail)),
                   projectPath,
                   error);
        result.errors.push_back(std::move(error));
        return finish(json, result, kExitFailed, projectPath);
    }
    const auto& project = *projectResult;
    for (std::size_t i = 0; i < project.loadWarnings.size(); ++i) {
        LintWarning warning{project.loadWarnings[i], {}};
        if (i < project.loadWarningSources.size()) {
            const auto& at = project.loadWarningSources[i];
            warning.at = Position{at.file, at.line, at.column, {}, {}};
        }
        result.warnings.push_back(std::move(warning));
    }
    if (project.id.empty() && !ce::collectSecretReferences(project).empty()) {
        result.warnings.push_back(LintWarning{
            std::format("project uses secrets but has no id, so they cannot be read from the "
                        "keychain (the CLI still reads them from REQLOOM_SECRET_<NAME>). Add "
                        "`id: {}` to reqloom.yaml.",
                        ce::generateProjectId()),
            Position{pathToUtf8(yamlPath), 0, 0, {}, {}}});
    }
    appendSecretNameWarnings(project, result.warnings);
    // A reference nothing extracts loads, but fails the run that reaches it.
    for (const auto& finding : ce::unproducedReferences(project)) {
        result.warnings.push_back(LintWarning{
            finding.line > 0 ? std::format("{}:{}: {}", finding.file, finding.line, finding.detail)
                             : finding.detail,
            positionOf(finding)});
    }
    // Load warnings (unknown keys, imports that matched nothing) never fail the lint.
    for (const auto& warning : result.warnings) {
        std::println(stderr, "  WARNING: {}", terminalSafe(warning.message));
        if (!warning.at.fix.empty()) {
            std::println(stderr, "    fix: {}", terminalSafe(warning.at.fix));
        }
    }
    std::size_t totalOps = 0;
    dryRunAll(project, result, totalOps);
    if (!result.errors.empty()) {
        std::println(stderr, "\nLINT FAILED — {} error(s).", result.errors.size());
        return finish(json, result, kExitFailed, projectPath);
    }
    if (!json) {
        std::println("LINT OK — {} actors, {} resources, {} operations. No errors.",
                     project.actors.size(),
                     project.resources.size(),
                     totalOps);
    }
    return finish(json, result, kExitSuccess, projectPath);
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
