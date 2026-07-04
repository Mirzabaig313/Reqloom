// ImportFromApidog — direct (non-LLM) parser that produces a Project from an
// Apidog native export (JSON, `apidogProject` + `apiCollection`).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromApidog {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse an Apidog native export into a Project.
    ///
    /// `apiCollection` is a folder tree; folders hold `items` (sub-folders or
    /// endpoints). Top-level folders become resources (Apidog's synthetic
    /// single "Root" wrapper is unwrapped so its child folders are the
    /// resources); each endpoint's `api` becomes an operation. Header/query
    /// parameter examples and a request-body example seed the operation. Apidog
    /// `{{var}}` references are rewritten to `{{env.var}}`. Apidog native
    /// exports carry no server URL, so `baseUrl` is left for the user to set.
    ///
    /// Path containment: the file must resolve to a regular file under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
