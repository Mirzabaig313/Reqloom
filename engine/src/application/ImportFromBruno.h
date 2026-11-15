// ImportFromBruno — parser for a Bruno collection (a directory of `.bru` files
// in Bruno's block DSL, marked by a `bruno.json`) → Project.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromBruno {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse a Bruno collection into a Project.
    /// `input` may be the collection directory, its `bruno.json`, or any `.bru` file inside it.
    /// Top-level sub-directories become resources, requests operations, `environments/*.bru`
    /// environments; `{{var}}` becomes `{{env.var}}`. Path containment: the collection root must
    /// resolve under `projectRoot`. Returns `SchemaInvalid` on unusable input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& input, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
