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
    ///
    /// Top-level folders become resources; nested folders fold into their
    /// top-level ancestor; each request becomes an operation. A request's
    /// `endpoint` scheme+host becomes the `default` environment's `baseUrl`.
    /// Hoppscotch `<<var>>` references are rewritten to `{{env.var}}`.
    ///
    /// Path containment: the export must resolve to a regular file under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
