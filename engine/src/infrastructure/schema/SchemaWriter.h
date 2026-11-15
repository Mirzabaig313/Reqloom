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
    /// When `overwrite` and `pruneStale` are both set (the in-place project
    /// save), `*.yaml` files in actors/, resources/ and environments/ that
    /// are not part of `project` are deleted. With `pruneStale` false the
    /// writer only replaces the files it writes and never deletes anything.
    ///
    /// Actor ids, resource ids and environment names become file names, so
    /// an unsafe one (empty, leading dot, `/`, `\`, `:`, NUL) is rejected and
    /// nothing is written.
    ///
    /// Layout produced:
    ///   targetDir/reqloom.yaml
    ///   targetDir/actors/<id>.yaml
    ///   targetDir/resources/<id>.yaml
    ///   targetDir/environments/<n>.yaml
    virtual SchemaWriteResult write(const std::filesystem::path& targetDir,
                                    const Project& project,
                                    bool overwrite = false,
                                    bool pruneStale = true) = 0;
};

}  // namespace reqloom::engine
