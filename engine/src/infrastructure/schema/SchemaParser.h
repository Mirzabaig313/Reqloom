// Engine-internal interface for parsing chainapi.yaml (and sub-files) into
// a validated Project. Concrete impl: YamlSchemaParser (yaml-cpp).
//
// Cycle detection, undefined-reference detection, and version validation
// happen here. Errors carry file:line context via `SchemaError::detail`.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace chainapi::engine {

/// Schema parse failure. `ChainApiError::detail` of the form
/// `"<path>:<line>: <message>"` so editors and humans both understand it.
using SchemaParseResult = std::expected<Project, ChainApiError>;

class SchemaParser {
public:
    virtual ~SchemaParser() = default;

    virtual SchemaParseResult parse(
        const std::filesystem::path& rootYaml) = 0;
};

}  // namespace chainapi::engine
