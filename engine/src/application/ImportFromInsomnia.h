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

        /// Multi-line warning string, one note per line. Empty when nothing was
        /// notable. Surfaced by the CLI verbatim and the desktop review UI.
        std::string warnings;
    };

    /// Parse `exportFile` (an Insomnia v4 JSON export) into a Project.
    ///
    /// Mapping: top-level request groups (children of the workspace) become
    /// resources; nested groups fold into their top-level ancestor; each request
    /// becomes an operation; environment `data` blocks seed the `default`
    /// environment; a request URL's scheme+host becomes the environment
    /// `baseUrl`. Insomnia `{{ _.var }}` / `{{var}}` references are rewritten to
    /// `{{env.var}}` so they resolve against that environment. Nunjucks tags
    /// (`{% ... %}`) have no reqloom equivalent and are flagged as warnings.
    ///
    /// Path containment: the export must resolve to a regular file under
    /// `projectRoot`, blocking `..` traversal.
    ///
    /// Returns `SchemaInvalid` when the document is not a recognisable Insomnia
    /// v4 export, or `YamlParse` for a JSON syntax error.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
