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
    ///
    /// Accepts the "Export Collection" shape (a JSON array of collection
    /// objects, each with `folders` + `requests`) and a single collection
    /// object. Top-level folders (containerId == "") become resources; nested
    /// folders fold into their top-level ancestor; each request becomes an
    /// operation. A request URL's scheme+host becomes the `default`
    /// environment's `baseUrl`; bare `{{var}}` references are rewritten to
    /// `{{env.var}}`.
    ///
    /// Path containment: the export must resolve to a regular file under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
