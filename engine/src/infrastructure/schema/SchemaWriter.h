// Engine-internal interface for writing a Project back to reqloom.yaml.
// Concrete impl: YamlSchemaWriter (yaml-cpp).
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>

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
    /// When `overwrite` and `pruneStale` are both set (the in-place project save), `*.yaml` files
    /// in actors/, resources/ and environments/ that are not part of `project` are deleted; with
    /// `pruneStale` false nothing is deleted.
    ///
    /// Actor ids, resource ids and environment names become file names, so an unsafe one (empty,
    /// leading dot, `/`, `\`, `:`, NUL) is rejected and nothing is written.
    ///
    /// `loadedFiles` (canonical path → hash of the bytes at load, as the parser records in
    /// `Project::loadedFiles`), when given, limits pruning to those files and fails the write
    /// before anything is committed if a target is on disk but not in it, or if a file in it
    /// changed since load. On success it is replaced by the files now on disk.
    ///
    /// Layout produced:
    ///   targetDir/reqloom.yaml
    ///   targetDir/actors/<id>.yaml
    ///   targetDir/resources/<id>.yaml
    ///   targetDir/environments/<n>.yaml
    virtual SchemaWriteResult write(
        const std::filesystem::path& targetDir,
        const Project& project,
        bool overwrite = false,
        bool pruneStale = true,
        std::map<std::filesystem::path, std::size_t>* loadedFiles = nullptr) = 0;
};

}  // namespace reqloom::engine
