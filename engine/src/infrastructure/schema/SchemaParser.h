// Engine-internal interface for parsing reqloom.yaml into a validated Project.
// Concrete impl: YamlSchemaParser (yaml-cpp).
// Errors carry file:line context via `ReqloomError::detail`.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace reqloom::engine {

/// The key `Project::loadedFiles` uses for `file`: symlinks resolved where the path exists,
/// so the parser and the writer agree on it however the project folder was named.
[[nodiscard]] inline std::filesystem::path canonicalSchemaPath(const std::filesystem::path& file) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(file, ec);
    if (ec) {
        return std::filesystem::absolute(file, ec).lexically_normal();
    }
    return canonical;
}

/// The hash `Project::loadedFiles` records for a file's bytes. Compared only within one process.
[[nodiscard]] inline std::size_t schemaFileHash(std::string_view bytes) {
    return std::hash<std::string_view>{}(bytes);
}

/// `ReqloomError::detail` is `"<path>:<line>:<column>: <message>"` (line and column when known).
using SchemaParseResult = std::expected<Project, ReqloomError>;

class SchemaParser {
public:
    SchemaParser() = default;
    SchemaParser(const SchemaParser&) = delete;
    SchemaParser& operator=(const SchemaParser&) = delete;
    SchemaParser(SchemaParser&&) = delete;
    SchemaParser& operator=(SchemaParser&&) = delete;
    virtual ~SchemaParser() = default;

    virtual SchemaParseResult parse(const std::filesystem::path& rootYaml) = 0;
};

}  // namespace reqloom::engine
