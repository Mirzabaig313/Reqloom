// ImportFromInsomnia — direct (non-LLM) parser that produces a Project from an
// Insomnia v4 export (JSON, __export_format 4).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromInsomnia {
public:
    struct Outcome {
        Project project;

        /// Multi-line warning string, one note per line; empty when nothing was notable.
        std::string warnings;
    };

    /// Parse `exportFile` (an Insomnia v4 JSON export) into a Project.
    /// Top-level request groups become resources (nested ones fold into them), requests operations,
    /// environment `data` blocks the `default` environment; `{{ _.var }}` / `{{var}}` become
    /// `{{env.var}}`. Nunjucks tags
    /// (`{% ... %}`) have no reqloom equivalent and are flagged as warnings. Path containment: the
    /// export must resolve under `projectRoot`. Returns `SchemaInvalid` for an unrecognisable
    /// export, `YamlParse` for a JSON syntax error.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
