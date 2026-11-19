// ProjectCommand — see header.
#include "ProjectCommand.h"

#include "ExitCodes.h"
#include "PathText.h"

#include "../output/Contract.h"
#include "../output/NextAction.h"
#include "../output/ReportWriter.h"
#include "../output/TerminalText.h"

#include <reqloom/engine/Factories.h>

#include <array>
#include <format>
#include <print>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

constexpr std::array<std::string_view, 1> kMachineFormats{"json"};

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

}  // namespace

void beginDocument(JsonWriter& json, std::string_view command, bool ok) {
    json.beginObject()
        .field("schema_version", kSchemaVersion)
        .field("contract_version", kContractVersion)
        .field("command", command)
        .field("ok", ok);
}

int deliverDocument(std::string_view command, const std::string& document, int exitCode) {
    if (const auto written = writeReportStream(stdout, document); !written) {
        std::println(stderr, "reqloom {}: cannot write the report: {}", command, written.error());
        return kExitSystem;
    }
    return exitCode;
}

int failCommand(const ProjectRequest& request,
                int exitCode,
                std::string_view code,
                std::string_view detail,
                const ce::ReqloomError* source) {
    std::println(stderr, "reqloom {}: [{}] {}", request.command, code, terminalSafe(detail));
    if (!request.json) {
        return exitCode;
    }
    JsonWriter json;
    beginDocument(json, request.command, false);
    json.key("error").beginObject();
    json.field("code", code).field("detail", detail);
    json.field("next_action", errorNextAction(code, request.command));
    json.fieldOrNull("file", source != nullptr ? std::string_view{source->file} : "");
    json.fieldOrNull("line",
                     source != nullptr && source->line > 0
                         ? std::optional<std::int64_t>{source->line}
                         : std::nullopt);
    json.endObject().endObject();
    return deliverDocument(request.command, json.str(), exitCode);
}

std::expected<ce::Project, int> loadProjectFor(const ProjectRequest& request) {
    const auto yamlPath = request.projectPath / "reqloom.yaml";
    std::error_code ec;
    if (!fs::exists(yamlPath, ec)) {
        return std::unexpected(
            failCommand(request,
                        kExitUsage,
                        kCodeProjectNotFound,
                        "reqloom.yaml not found in " + pathToUtf8(request.projectPath)));
    }
    auto project = ce::parseProject(yamlPath);
    if (!project) {
        const auto& error = project.error();
        return std::unexpected(
            failCommand(request, kExitFailed, ce::toCodeString(error.code), error.detail, &error));
    }
    if (!request.environment.empty() && !project->environments.contains(request.environment) &&
        request.environment != project->defaultEnvironment) {
        return std::unexpected(
            failCommand(request,
                        kExitUsage,
                        kCodeUnknownEnvironment,
                        std::format("unknown environment '{}'; the project has: {}",
                                    request.environment,
                                    environmentList(*project))));
    }
    return std::move(*project);
}

std::expected<ProjectRequest, std::string> projectRequestFrom(std::string command,
                                                              const ParsedArguments& parsed) {
    ProjectRequest request;
    request.command = std::move(command);
    const auto format = parsed.value("--format").value_or("text");
    if (format != "text" && format != "json") {
        return std::unexpected(
            std::format("--format must be one of text, json (got '{}')", format));
    }
    request.json = format == "json";
    if (const auto project = parsed.value("--project")) {
        request.projectPath = pathFromUtf8(*project);
    }
    request.environment = parsed.value("--env").value_or("");
    return request;
}

int usageFailure(std::string command, const QStringList& args, const std::string& message) {
    ProjectRequest request;
    request.json = peekFormat(args, kMachineFormats).has_value();
    request.command = std::move(command);
    const int code = failCommand(request, kExitUsage, kCodeUsage, message);
    std::println(stderr, "Run 'reqloom {} --help' for usage.", request.command);
    return code;
}

std::string projectRelative(const ce::Project& project, const std::string& file) {
    if (file.empty() || project.rootDir.empty()) {
        return file;
    }
    std::error_code ec;
    const auto relative = fs::relative(pathFromUtf8(file), project.rootDir, ec);
    if (ec || relative.empty() || *relative.begin() == "..") {
        return file;
    }
    return relative.generic_string();
}

}  // namespace reqloom::cli
