// Process exit codes shared by every `reqloom` command.
#pragma once

namespace reqloom::cli {

inline constexpr int kExitSuccess = 0;
/// The run or the project failed: a step or assertion failed, the project does not validate, an
/// import could not be converted.
inline constexpr int kExitFailed = 1;
/// Invalid invocation, including a named project, operation, environment or file that does not
/// exist.
inline constexpr int kExitUsage = 2;
/// A file or system error: an output cannot be written, an unexpected exception.
inline constexpr int kExitSystem = 3;

/// Error codes for failures the CLI detects itself, in reports next to the engine's codes.
inline constexpr const char* kCodeUsage = "E_CLI_USAGE";
inline constexpr const char* kCodeProjectNotFound = "E_CLI_PROJECT_NOT_FOUND";
inline constexpr const char* kCodeUnknownOperation = "E_CLI_UNKNOWN_OPERATION";
inline constexpr const char* kCodeUnknownEnvironment = "E_CLI_UNKNOWN_ENVIRONMENT";
inline constexpr const char* kCodeInternal = "E_CLI_INTERNAL";
/// `show`: no recorded run with that id (or no run recorded yet for `last`).
inline constexpr const char* kCodeUnknownRun = "E_CLI_UNKNOWN_RUN";
/// `show`: the run has no step with that index or operation id.
inline constexpr const char* kCodeUnknownStep = "E_CLI_UNKNOWN_STEP";
/// `show`: the run history for this project cannot be opened, or holds no run.
inline constexpr const char* kCodeNoHistory = "E_CLI_NO_HISTORY";
/// `show --path`: the JSONPath selected nothing, or the recorded body cannot be searched.
inline constexpr const char* kCodePathNotFound = "E_CLI_PATH_NOT_FOUND";

}  // namespace reqloom::cli
