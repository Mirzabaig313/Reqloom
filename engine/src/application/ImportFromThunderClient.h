// ImportFromThunderClient — direct (non-LLM) parser that produces a Project
// from a Thunder Client (VS Code) collection export (JSON).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromThunderClient {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse a Thunder Client collection export into a Project.
    /// Accepts the "Export Collection" array of collections or a single collection object.
    /// Top-level folders (containerId == "") become resources (nested ones fold into them),
    /// requests operations; `{{var}}` becomes
    /// `{{env.var}}`. Path containment: the export must resolve under `projectRoot`.
    /// Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
