// Engine-internal interface for writing a Project back to reqloom.yaml.
// Concrete impl: YamlSchemaWriter (yaml-cpp).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>

namespace reqloom::engine {

/// Returns the written root yaml path on success.
using SchemaWriteResult = std::expected<std::filesystem::path, ReqloomError>;

class SchemaWriter {
public:
    SchemaWriter() = default;
    SchemaWriter(const SchemaWriter&) = delete;
    SchemaWriter& operator=(const SchemaWriter&) = delete;
    SchemaWriter(SchemaWriter&&) = delete;
    SchemaWriter& operator=(SchemaWriter&&) = delete;
    virtual ~SchemaWriter() = default;

    /// Write `project` into `targetDir`. Creates the directory if needed,
    /// refuses to overwrite existing files unless `overwrite` is set.
    ///
    /// Layout produced:
    ///   targetDir/reqloom.yaml
    ///   targetDir/actors/<id>.yaml
    ///   targetDir/resources/<id>.yaml
    ///   targetDir/environments/<n>.yaml
    virtual SchemaWriteResult write(const std::filesystem::path& targetDir,
                                    const Project& project,
                                    bool overwrite = false) = 0;
};

}  // namespace reqloom::engine
