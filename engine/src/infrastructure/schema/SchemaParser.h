// Engine-internal interface for parsing chainapi.yaml into a validated Project.
// Concrete impl: YamlSchemaParser (yaml-cpp).
// Errors carry file:line context via `ChainApiError::detail`.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace chainapi::engine {

/// `ChainApiError::detail` is of the form `"<path>:<line>: <message>"`.
using SchemaParseResult = std::expected<Project, ChainApiError>;

class SchemaParser {
public:
    virtual ~SchemaParser() = default;

    virtual SchemaParseResult parse(const std::filesystem::path& rootYaml) = 0;
};

}  // namespace chainapi::engine
