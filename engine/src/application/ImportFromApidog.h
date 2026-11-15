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
    /// Top-level folders become resources (the synthetic "Root" wrapper is unwrapped), each
    /// endpoint's `api` an operation; `{{var}}` becomes `{{env.var}}`. The export carries no server
    /// URL, so `baseUrl` is left unset. Path containment: the file must resolve under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
