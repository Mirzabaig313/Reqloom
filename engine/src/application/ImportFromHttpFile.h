// ImportFromHttpFile — direct parser for `.http` / `.rest` files (the VS Code
// REST Client and JetBrains HTTP Client format) → Project.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromHttpFile {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse a `.http` / `.rest` file into a Project.
    /// Requests are separated by `###`; each is `METHOD URL`, `Header: Value` lines, a blank line,
    /// a raw body.
    /// `@name = value` definitions seed the `default` environment; `{{var}}` becomes `{{env.var}}`.
    /// All requests land in one resource named after the file. Path containment: the file must
    /// resolve under `projectRoot`. Returns `SchemaInvalid` on unusable input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& file, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
