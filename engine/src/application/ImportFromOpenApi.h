// ImportFromOpenApi — direct (non-LLM) parser that produces a Project from
// an OpenAPI 3.x document. Slice 6c-1: skeleton path/method walker.
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
    /// Path containment: the file must be either an absolute path or
    /// a path inside the current working directory. `..` traversal that
    /// resolves outside the cwd is rejected.
    ///
    /// Returns `SchemaInvalid` for malformed input (not a YAML map, missing
    /// `openapi` field, no `paths`, etc.) or `IoError` for file-system
    /// problems.
    [[nodiscard]] std::expected<Outcome, ChainApiError>
    run(const std::filesystem::path& spec) const;
};

}  // namespace chainapi::engine
