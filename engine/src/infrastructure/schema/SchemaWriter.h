// Engine-internal interface for writing a Project back to chainapi.yaml.
// Concrete impl: YamlSchemaWriter (yaml-cpp).
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>

namespace chainapi::engine {

/// Returns the written root yaml path on success.
using SchemaWriteResult = std::expected<std::filesystem::path, ChainApiError>;

class SchemaWriter {
public:
    virtual ~SchemaWriter() = default;

    /// Write `project` into `targetDir`. Creates the directory if needed,
    /// refuses to overwrite existing files unless `overwrite` is set.
    ///
    /// Layout produced:
    ///   targetDir/chainapi.yaml
    ///   targetDir/actors/<id>.yaml
    ///   targetDir/resources/<id>.yaml
    ///   targetDir/environments/<n>.yaml
    virtual SchemaWriteResult write(const std::filesystem::path& targetDir,
                                    const Project& project,
                                    bool overwrite = false) = 0;
};

}  // namespace chainapi::engine
