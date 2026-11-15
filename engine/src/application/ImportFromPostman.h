// ImportFromPostman — direct (non-LLM) parser that produces a Project from a
// Postman Collection v2.1 export (JSON).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/ImportCredentials.h>

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace reqloom::engine {

class ImportFromPostman {
public:
    struct Outcome {
        Project project;

        /// Multi-line warning string, one note per line; empty when nothing was notable.
        std::string warnings;

        /// Collection variables Postman marks `type: "secret"`.
        std::vector<SourceSecretVariable> secretVariables;
    };

    /// Parse `collection` (a Postman Collection v2.1 JSON export) into a Project.
    /// Top-level folders become resources, requests operations, the `variable` block the `default`
    /// environment, and a request URL's scheme+host the environment `baseUrl`. `{{var}}` becomes
    /// `{{env.var}}`. Path containment: the file must resolve under `projectRoot`. Returns
    /// `SchemaInvalid` for an unrecognisable or unsupported-version collection, `YamlParse` for a
    /// JSON syntax error.
    [[nodiscard]] std::expected<Outcome, ReqloomError> run(
        const std::filesystem::path& collection, const std::filesystem::path& projectRoot) const;
};

}  // namespace reqloom::engine
