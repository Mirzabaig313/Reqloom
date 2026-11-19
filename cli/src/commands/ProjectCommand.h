// Shared plumbing for the read-only project commands (`list`, `describe`, `plan`, `show`): the
// common options, loading the project, and the error document every JSON output ends in when the
// command cannot answer (PRD FR-13.11).
#pragma once

#include "ArgParser.h"

#include "../output/JsonWriter.h"

#include <reqloom/engine/PublicApi.h>

#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// What every project command was asked for.
struct ProjectRequest {
    std::string command;  ///< `list`, `describe`, …; names the command in messages and documents.
    bool json{false};
    std::filesystem::path projectPath{std::filesystem::current_path()};
    std::string environment;  ///< Empty: the project default.
};

/// The JSON head every command document starts with: `schema_version`, `contract_version`,
/// `command`, `ok`. The caller adds its members and closes the object.
void beginDocument(JsonWriter& json, std::string_view command, bool ok);

/// Print `detail` on stderr and, in JSON, write the error document on stdout:
/// `{ schema_version, contract_version, command, ok: false, error: { code, detail, next_action,
/// file, line } }`. Returns `exitCode`, or kExitSystem when the document could not be written.
[[nodiscard]] int failCommand(const ProjectRequest& request,
                              int exitCode,
                              std::string_view code,
                              std::string_view detail,
                              const engine::ReqloomError* source = nullptr);

/// Write `document` to stdout; `exitCode`, or kExitSystem when it could not be written.
[[nodiscard]] int deliverDocument(std::string_view command,
                                  const std::string& document,
                                  int exitCode);

/// Load the project, or fail as `failCommand` does (no reqloom.yaml: exit 2; does not validate:
/// exit 1, the error's code and position). Checks `request.environment` exists as well.
[[nodiscard]] std::expected<engine::Project, int> loadProjectFor(const ProjectRequest& request);

/// `request` from parsed options `--project`, `--env` and `--format text|json`.
[[nodiscard]] std::expected<ProjectRequest, std::string> projectRequestFrom(
    std::string command, const ParsedArguments& parsed);

/// Usage error: message on stderr, the error document in JSON when `--format json` was asked for.
[[nodiscard]] int usageFailure(std::string command,
                               const QStringList& args,
                               const std::string& message);

/// The project-relative form of a schema file path for compact output (`resources/order.yaml`);
/// the path as given when it is outside the project.
[[nodiscard]] std::string projectRelative(const engine::Project& project, const std::string& file);

}  // namespace reqloom::cli
