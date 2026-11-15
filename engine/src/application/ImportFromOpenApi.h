// ImportFromOpenApi — direct (non-LLM) parser that produces a Project from
// an OpenAPI 3.x document.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromOpenApi {
public:
    struct Outcome {
        Project project;

        /// Multi-line warning string; empty when nothing was notable. Surfaced verbatim by the CLI
        /// and desktop review UI.
        std::string warnings;
    };

    /// Parse `spec` (YAML or JSON, OpenAPI 3.0.x or 3.1.x) into a Project.
    /// Path containment: the spec must resolve under `projectRoot`; anything outside
    /// (`/etc/passwd`, `..` traversal) is rejected. Returns `SchemaInvalid` for malformed input,
    /// `YamlParse` for a syntax error.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& spec, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
