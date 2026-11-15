// ImportFromHttpie — direct (non-LLM) parser that produces a Project from an
// HTTPie client export (JSON: {meta:{format:"httpie",...}, entry:<...>}).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/ImportCredentials.h>

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace reqloom::engine {

class ImportFromHttpie {
public:
    struct Outcome {
        Project project;
        std::string warnings;

        /// Environment variables HTTPie marks `isSecret`.
        std::vector<SourceSecretVariable> secretVariables;
    };

    /// Parse an HTTPie export into a Project.
    /// The export wraps an `entry` that is a Workspace, Collection, Request or Environment.
    /// Workspace collections become resources, requests operations, workspace environments
    /// environments (the default wins); `{{var}}` becomes `{{env.var}}`. Path containment: the file
    /// must resolve under `projectRoot`. Returns `SchemaInvalid`/`YamlParse` on bad input.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& exportFile, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
