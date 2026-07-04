// ImportFromHttpie — direct (non-LLM) parser that produces a Project from an
// HTTPie client export (JSON: {meta:{format:"httpie",...}, entry:<...>}).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

class ImportFromHttpie {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    /// Parse an HTTPie export into a Project.
    ///
    /// The export wraps an `entry` that is a Workspace, Collection, Request, or
    /// Environment. Workspace collections become resources; each request becomes
    /// an operation; workspace environments seed environments (the default one
    /// wins); a request URL's scheme+host becomes `baseUrl`. HTTPie `{{var}}`
    /// references are rewritten to `{{env.var}}`.
    ///
    /// Path containment: the file must resolve to a regular file under
    /// `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
