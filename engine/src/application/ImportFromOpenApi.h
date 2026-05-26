// ImportFromOpenApi — direct (non-LLM) parser that produces a Project from
// an OpenAPI 3.x document.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace chainapi::engine {

class ImportFromOpenApi {
public:
    struct Outcome {
        Project project;

        /// Multi-line warning string. Empty when the import had nothing
        /// notable to flag. Surfaced by the CLI verbatim and by the desktop
        /// importer review UI line-by-line.
        std::string warnings;
    };

    /// Parse `spec` (YAML or JSON, OpenAPI 3.0.x or 3.1.x) into a Project.
    ///
    /// Path containment: the spec must resolve to a regular file under
    /// `projectRoot`. The importer rejects anything outside that root —
    /// blocks `--spec /etc/passwd` style invocations and `..` traversal
    /// out of the project. CLI/desktop callers typically pass the
    /// directory that will hold the generated `chainapi.yaml`; tests
    /// pass a scratch dir.
    ///
    /// Returns `SchemaInvalid` for malformed input (not a YAML map, missing
    /// `openapi` field, no `paths`, etc.) or `YamlParse` for a YAML/JSON
    /// syntax error.
    [[nodiscard]] std::expected<Outcome, ChainApiError> run(
        const std::filesystem::path& spec, const std::filesystem::path& projectRoot) const;
};

}  // namespace chainapi::engine
