// ImportFromPostman — direct (non-LLM) parser that produces a Project from a
// Postman Collection v2.1 export (JSON).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromPostman {
public:
    struct Outcome {
        Project project;

        /// Multi-line warning string, one note per line. Empty when the import
        /// had nothing notable to flag. Surfaced by the CLI verbatim and by the
        /// desktop importer review UI line-by-line.
        std::string warnings;
    };

    /// Parse `collection` (a Postman Collection v2.1 JSON export) into a
    /// Project.
    ///
    /// Mapping: top-level folders become resources, each request becomes an
    /// operation, the collection's `variable` block seeds the `default`
    /// environment, and a request URL's scheme+host becomes the environment
    /// `baseUrl` (with the path left as the operation's path template). Bare
    /// Postman `{{var}}` references are rewritten to `{{env.var}}` so they
    /// resolve against that environment.
    ///
    /// Path containment: the collection file must resolve to a regular file
    /// under `projectRoot`, blocking `..` traversal out of the project.
    ///
    /// Returns `SchemaInvalid` when the document is not a recognisable Postman
    /// v2.1 collection (missing `info`/`item`, or the schema URL names an
    /// unsupported version), or `YamlParse` for a JSON syntax error.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& collection, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
