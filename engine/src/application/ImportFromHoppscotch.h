// ImportFromHoppscotch — direct (non-LLM) parser that produces a Project from a
// Hoppscotch collection export (JSON).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromHoppscotch {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse a Hoppscotch collection export (JSON) into a Project.
    /// Top-level folders become resources (nested ones fold into them), requests operations;
    /// `<<var>>` becomes `{{env.var}}`. Path containment: the export must resolve under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
