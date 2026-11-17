// `reqloom run <op>` — load a project, resolve a chain, execute,
// and emit results through the requested renderer (text / json / junit).

#include "RunCommand.h"

#include "ArgParser.h"
#include "ByteSizeArgument.h"
#include "ExitCodes.h"
#include "HostApprovalPrompt.h"
#include "PathText.h"
#include "RunInterrupt.h"

#include "../output/JUnitRenderer.h"
#include "../output/JsonRenderer.h"
#include "../output/ReportWriter.h"
#include "../output/TextRenderer.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

enum class Format : std::uint8_t { Text, Json, JUnit };

constexpr std::array<std::string_view, 2> kMachineFormats{"json", "junit"};

constexpr std::array kOptions{
    OptionSpec{.name = "--project", .takesValue = true},
    OptionSpec{.name = "--env", .takesValue = true},
    OptionSpec{.name = "--var", .takesValue = true, .repeatable = true},
    OptionSpec{.name = "--format", .takesValue = true},
    OptionSpec{.name = "--output", .takesValue = true},
    OptionSpec{.name = "--quiet"},
    OptionSpec{.name = "--failure-body-size", .takesValue = true},
    OptionSpec{.name = "--response-body-size", .takesValue = true},
    OptionSpec{.name = "--non-interactive"},
};

/// Most operation names listed when the target does not exist.
constexpr std::size_t kListedOperations = 50;

struct RunArgs {
    std::string targetOp;
    fs::path projectPath{fs::current_path()};
    std::string envName;
    std::map<std::string, std::string> overrides;
    Format format{Format::Text};
    fs::path outputPath;  ///< empty = stdout
    bool quiet{false};
    std::optional<std::size_t> failureExcerptBytes;
    std::optional<std::size_t> responseBodyBytes;
    bool nonInteractive{false};
    /// Hosts `REQLOOM_APPROVED_HOSTS` approves for this run.
    std::vector<std::string> approvedHosts;
};

void printUsage(std::FILE* stream) {
    std::print(
        stream,
        "Usage: reqloom run <resource.operation> [options]\n"
        "Options may come before or after the operation; --name=value also works, and\n"
        "-- ends the options.\n"
        "  --project <path>     Project directory (default: cwd)\n"
        "  --env <name>         Environment to run against (default: the project's)\n"
        "  --var KEY=VALUE      Override a variable of that environment (repeatable)\n"
        "  --format <fmt>       Output format: text (default), json, junit\n"
        "  --output <file>      Write the report to <file> (default: stdout); replaced only\n"
        "                       by a complete new report\n"
        "  --quiet              Suppress live progress on stdout (failures still go\n"
        "                       to stderr). Implied by --format json|junit.\n"
        "  --failure-body-size <size>\n"
        "                       Most of the response body quoted in a failure message,\n"
        "                       as bytes or with KiB/MiB (default 4KiB; 0 = none;\n"
        "                       capped at 1MiB). Secrets are masked at any size.\n"
        "  --response-body-size <size>\n"
        "                       With --format json, most of each step's response body\n"
        "                       kept in its `response` field (default 2KiB; 0 = none;\n"
        "                       capped at 1MiB). Masked before it is cut; never unmasked.\n"
        "  --non-interactive    Never ask to approve a host, even at a terminal\n"
        "Credentials, secrets and session cookies go only to hosts a person approved for\n"
        "this project. At a terminal, run asks (yes/no) the first time; with no terminal\n"
        "it never asks and the run fails before sending. CI approves hosts for a run with\n"
        "REQLOOM_APPROVED_HOSTS=[http://|https://]host[:port],... (no scheme means https).\n"
        "With --format json or junit, stdout (or --output) receives exactly one document\n"
        "for every outcome, errors before the first step included; diagnostics go to stderr.\n"
        "Exit codes:\n"
        "  0    the run succeeded\n"
        "  1    the run failed (a step or assertion failed), the project does not validate,\n"
        "       or the engine refused the run\n"
        "  2    invalid invocation: unknown option, missing or invalid value, no reqloom.yaml\n"
        "       in the project path, unknown operation or environment\n"
        "  3    file or system error: the report cannot be written to --output or stdout,\n"
        "       or an unexpected error\n"
        "  130  interrupted by SIGINT (Ctrl-C), 143 terminated by SIGTERM: the run is\n"
        "       cancelled and its report written; a second signal exits at once\n");
}

[[nodiscard]] std::expected<Format, std::string> parseFormat(const std::string& token) {
    if (token == "text") {
        return Format::Text;
    }
    if (token == "json") {
        return Format::Json;
    }
    if (token == "junit") {
        return Format::JUnit;
    }
    return std::unexpected("--format must be one of text, json, junit (got '" + token + "')");
}

[[nodiscard]] std::expected<void, std::string> parseVars(const ParsedArguments& parsed,
                                                         RunArgs& out) {
    for (const auto& kv : parsed.values("--var")) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) {
            return std::unexpected(
                std::format("--var needs KEY=VALUE with a non-empty KEY, got '{}'", kv));
        }
        out.overrides[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    return {};
}

/// `flag`'s byte size into `target`, or an error naming the flag.
[[nodiscard]] std::expected<void, std::string> parseSize(const ParsedArguments& parsed,
                                                         const std::string_view flag,
                                                         std::optional<std::size_t>& target) {
    if (const auto size = parsed.value(flag)) {
        target = parseByteSizeArgument(*size);
        if (!target) {
            return std::unexpected(std::format(
                "{} must be a byte count or a size in KiB or MiB such as 16KiB, got '{}'",
                flag,
                *size));
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> parseSizes(const ParsedArguments& parsed,
                                                          RunArgs& out) {
    if (auto failure = parseSize(parsed, "--failure-body-size", out.failureExcerptBytes);
        !failure) {
        return failure;
    }
    return parseSize(parsed, "--response-body-size", out.responseBodyBytes);
}

[[nodiscard]] std::expected<RunArgs, std::string> toRunArgs(const ParsedArguments& parsed) {
    if (parsed.positionals.empty()) {
        return std::unexpected(std::string{"missing <operation>"});
    }
    if (parsed.positionals.size() > 1) {
        return std::unexpected(std::format("unexpected argument '{}'", parsed.positionals[1]));
    }
    RunArgs out;
    out.targetOp = parsed.positionals.front();
    if (const auto project = parsed.value("--project")) {
        out.projectPath = pathFromUtf8(*project);
    }
    out.envName = parsed.value("--env").value_or("");
    if (auto vars = parseVars(parsed, out); !vars) {
        return std::unexpected(vars.error());
    }
    if (const auto format = parsed.value("--format")) {
        auto fmt = parseFormat(*format);
        if (!fmt) {
            return std::unexpected(fmt.error());
        }
        out.format = *fmt;
    }
    if (const auto output = parsed.value("--output")) {
        out.outputPath = pathFromUtf8(*output);
    }
    if (auto sizes = parseSizes(parsed, out); !sizes) {
        return std::unexpected(sizes.error());
    }
    // Machine-readable formats imply --quiet: stdout carries a single document.
    out.quiet = parsed.has("--quiet") || out.format != Format::Text;
    out.nonInteractive = parsed.has("--non-interactive");
    return out;
}

/// Where the report goes, and the error document for a run that ends before its first step.
struct Report {
    Format format{Format::Text};
    fs::path output;
    std::string target;
    std::string environment;
    std::optional<ce::RunId> runId;

    /// Write `document`; `exitCode` on success, kExitSystem if it could not be written.
    [[nodiscard]] int deliver(const std::string& document, int exitCode) const {
        const auto written = output.empty() ? writeReportStream(stdout, document)
                                            : writeReportFile(output, document);
        if (!written) {
            std::println(stderr, "reqloom run: cannot write the report: {}", written.error());
            return kExitSystem;
        }
        return exitCode;
    }

    /// Print `message` on stderr and, in a machine format, deliver an error document.
    [[nodiscard]] int fail(int exitCode,
                           std::string_view code,
                           std::string_view detail,
                           std::string_view message) const {
        std::println(stderr, "{}", message);
        if (format == Format::Text) {
            return exitCode;
        }
        std::ostringstream doc;
        const ce::OperationId op{target};
        if (format == Format::Json) {
            JsonRenderer{doc}.renderError(op, environment, runId, code, detail);
        } else {
            JUnitRenderer{doc}.renderError(op, environment, runId, code, detail);
        }
        return deliver(doc.str(), exitCode);
    }

    [[nodiscard]] int failWith(int exitCode, const ce::ReqloomError& error, std::string_view what) {
        const auto code = ce::toCodeString(error.code);
        return fail(
            exitCode, code, error.detail, std::format("{} [{}]: {}", what, code, error.detail));
    }
};

[[nodiscard]] int usageError(const QStringList& args, const std::string& message) {
    Report report;
    if (const auto format = peekFormat(args, kMachineFormats)) {
        report.format = *format == "json" ? Format::Json : Format::JUnit;
    }
    const int code =
        report.fail(kExitUsage, kCodeUsage, message, std::format("reqloom run: {}", message));
    std::println(stderr, "Run 'reqloom run --help' for usage.");
    return code;
}

[[nodiscard]] bool operationExists(const ce::Project& project, const std::string& id) {
    const auto dot = id.find('.');
    if (dot == std::string::npos) {
        return false;
    }
    const auto resource = project.resources.find(ce::ResourceId{id.substr(0, dot)});
    return resource != project.resources.end() &&
           resource->second.operations.contains(id.substr(dot + 1));
}

[[nodiscard]] std::string operationList(const ce::Project& project) {
    std::string names;
    std::size_t count = 0;
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& op : resource.operations | std::views::values) {
            if (count++ < kListedOperations) {
                names += (names.empty() ? "" : ", ") + op.id.value;
            }
        }
    }
    if (count > kListedOperations) {
        names += std::format(" and {} more", count - kListedOperations);
    }
    return names.empty() ? std::string{"none"} : names;
}

/// The project's environment names; the default is always one, even with no variables.
[[nodiscard]] std::string environmentList(const ce::Project& project) {
    std::string names;
    bool hasDefault = false;
    for (const auto& name : project.environments | std::views::keys) {
        names += (names.empty() ? "" : ", ") + name;
        hasDefault = hasDefault || name == project.defaultEnvironment;
    }
    if (!hasDefault) {
        names += (names.empty() ? "" : ", ") + project.defaultEnvironment;
    }
    return names;
}

[[nodiscard]] std::expected<ce::Project, int> loadProject(const RunArgs& cfg, Report& report) {
    const auto yamlPath = cfg.projectPath / "reqloom.yaml";
    std::error_code ec;
    if (!fs::exists(yamlPath, ec)) {
        const auto detail = "reqloom.yaml not found in " + pathToUtf8(cfg.projectPath);
        return std::unexpected(
            report.fail(kExitUsage, kCodeProjectNotFound, detail, "Error: " + detail));
    }
    auto project = ce::parseProject(yamlPath);
    if (!project) {
        return std::unexpected(report.failWith(kExitFailed, project.error(), "Schema error"));
    }
    return std::move(*project);
}

/// Exit code of a target or environment that does not exist in `project`, or nullopt.
[[nodiscard]] std::optional<int> checkNames(const RunArgs& cfg,
                                            const ce::Project& project,
                                            const Report& report) {
    if (!operationExists(project, cfg.targetOp)) {
        const auto detail = std::format(
            "unknown operation '{}'; the project has: {}", cfg.targetOp, operationList(project));
        return report.fail(kExitUsage, kCodeUnknownOperation, detail, "Error: " + detail);
    }
    if (!project.environments.contains(report.environment) &&
        report.environment != project.defaultEnvironment) {
        const auto detail = std::format("unknown environment '{}'; the project has: {}",
                                        report.environment,
                                        environmentList(project));
        return report.fail(kExitUsage, kCodeUnknownEnvironment, detail, "Error: " + detail);
    }
    return std::nullopt;
}

[[nodiscard]] ce::RunOptions runOptions(const RunArgs& cfg) {
    ce::RunOptions options;
    options.environment = cfg.envName;
    if (cfg.failureExcerptBytes) {
        options.failureExcerptBytes = *cfg.failureExcerptBytes;
    }
    // Every CLI format can reach an agent: agent masking always, secrets from
    // REQLOOM_SECRET_<NAME> before the keychain. Only the JSON document has a field for the
    // response; the engine masks it before returning.
    options.agentOutput = true;
    options.secretsFromEnvironment = true;
    options.recordResponses = cfg.format == Format::Json;
    options.responseBodyBytes = cfg.responseBodyBytes.value_or(ce::kDefaultResponseBodyBytes);
    return options;
}

[[nodiscard]] std::string renderRun(const RunArgs& cfg,
                                    const Report& report,
                                    TextRenderer& text,
                                    const std::ostringstream& summary,
                                    const ce::RunResult& result) {
    const ce::OperationId target{cfg.targetOp};
    std::ostringstream doc;
    switch (cfg.format) {
        case Format::Text:
            text.render(target, report.environment, result);
            return summary.str();
        case Format::Json:
            JsonRenderer{doc}.render(target, report.environment, result);
            break;
        case Format::JUnit:
            JUnitRenderer{doc}.render(target, report.environment, result);
            break;
    }
    return doc.str();
}

[[nodiscard]] int runAndReport(const RunArgs& cfg, const ce::Project& project, Report& report) {
    std::ostringstream summary;
    TextRenderer text(summary, std::cout, std::cerr, cfg.quiet);
    if (cfg.format == Format::Text) {
        text.printProjectPreamble(project.name, project.actors.size(), project.resources.size());
    }

    // Host approval (FR-15.9). Asking needs a terminal and the user setting that lets the CLI
    // approve; `REQLOOM_APPROVED_HOSTS` obeys the same setting, so turning CLI approval off leaves
    // the desktop app as the only way to approve.
    const auto approvals = ce::makeFileHostApprovalStore();
    const bool cliMayApprove = approvals->cliApprovalAllowed().value_or(false);
    TerminalHostApprover approver{
        !cfg.nonInteractive && terminalAttached(), cliMayApprove, stdin, stderr};
    auto options = runOptions(cfg);
    options.hostApprover = &approver;
    if (cliMayApprove) {
        options.approvedHostsForRun = cfg.approvedHosts;
    } else if (!cfg.approvedHosts.empty()) {
        std::println(stderr,
                     "warning: {} is ignored: approving hosts from the command line is turned "
                     "off on this machine",
                     kApprovedHostsVariable);
    }

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    RunInterrupt interrupt{[&engine](ce::RunId id) {
        engine.cancel(id);
    }};
    engine.subscribe([&](const ce::RunEvent& event) {
        if (const auto* started = std::get_if<ce::RunStarted>(&event)) {
            report.runId = started->runId;
            interrupt.runStarted(started->runId);
        }
        if (cfg.format == Format::Text) {
            text.onEvent(event);
        }
    });

    ce::RunContext ctx;
    const auto result = engine.run(project, ce::OperationId{cfg.targetOp}, ctx, options);
    const int code = result ? report.deliver(renderRun(cfg, report, text, summary, *result),
                                             result->succeeded() ? kExitSuccess : kExitFailed)
                            : report.failWith(kExitFailed, result.error(), "Engine error");
    // A signal's code wins over the run's outcome, but not over a report that was not written.
    const int signalCode = interrupt.exitCode();
    return code == kExitSystem || signalCode == 0 ? code : signalCode;
}

[[nodiscard]] int execute(RunArgs cfg, Report& report) {
    auto approved = hostsApprovedByEnvironment();
    if (!approved) {
        return report.fail(kExitUsage, kCodeUsage, approved.error(), "Error: " + approved.error());
    }
    cfg.approvedHosts = std::move(*approved);
    auto project = loadProject(cfg, report);
    if (!project) {
        return project.error();
    }
    report.environment = cfg.envName.empty() ? project->defaultEnvironment : cfg.envName;
    if (const auto failed = checkNames(cfg, *project, report)) {
        return *failed;
    }
    auto& envVars = project->environments[report.environment];
    for (const auto& [key, value] : cfg.overrides) {
        envVars[key] = value;
    }
    return runAndReport(cfg, *project, report);
}

}  // namespace

int runCommand(const QStringList& args) {
    const auto parsed = parseArguments(args, kOptions);
    if (parsed && parsed->help) {
        printUsage(stdout);
        return kExitSuccess;
    }
    const auto cfg = parsed ? toRunArgs(*parsed)
                            : std::expected<RunArgs, std::string>{std::unexpect, parsed.error()};
    if (!cfg) {
        return usageError(args, cfg.error());
    }
    Report report{cfg->format, cfg->outputPath, cfg->targetOp, cfg->envName, std::nullopt};
    try {
        return execute(*cfg, report);
    } catch (const std::exception& ex) {
        return report.fail(kExitSystem,
                           kCodeInternal,
                           ex.what(),
                           std::format("Error: unexpected failure: {}", ex.what()));
    }
}

}  // namespace reqloom::cli
