// Engine-internal interface for parsing reqloom.yaml into a validated Project.
// Concrete impl: YamlSchemaParser (yaml-cpp).
// Errors carry file:line context via `ReqloomError::detail`.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace reqloom::engine {

/// `ReqloomError::detail` is of the form `"<path>:<line>: <message>"`.
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
