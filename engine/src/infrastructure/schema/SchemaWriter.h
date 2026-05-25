// Engine-internal interface for writing a Project back to a chainapi.yaml
// (or layered set of files). Concrete impl: YamlSchemaWriter (yaml-cpp).
//
// Importers (PRD §10) are the primary consumers — after the AI / OpenAPI /
// Postman importer produces a Project value, the writer persists it to a
// directory the user picked. Provenance metadata (PRD §10.3.3) round-trips
// intact so the runtime diagnostics in §10.3.4 keep working after a
// reload.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>

namespace chainapi::engine {

/// Result type. Returns the written root yaml path on success (useful for
/// log messages and editor follow-up), or a ChainApiError detailing what
/// went wrong (filesystem error, malformed project, etc.).
using SchemaWriteResult = std::expected<std::filesystem::path, ChainApiError>;

class SchemaWriter {
public:
    virtual ~SchemaWriter() = default;

    /// Write `project` into a directory rooted at `targetDir`. The
    /// writer creates the directory if it does not exist, refuses to
    /// overwrite existing files unless `overwrite` is set, and returns
    /// the path to the produced `chainapi.yaml`.
    ///
    /// Layout produced (matches what the parser expects):
    ///   targetDir/chainapi.yaml         ← root with name + imports list
    ///   targetDir/actors/<id>.yaml      ← one file per actor
    ///   targetDir/resources/<id>.yaml   ← one file per resource
    ///   targetDir/environments/<n>.yaml ← one file per environment
    ///
    /// Implementations must round-trip: writing then re-parsing the same
    /// project yields a structurally equivalent Project (modulo map
    /// ordering, which YAML does not guarantee).
    virtual SchemaWriteResult write(const std::filesystem::path& targetDir,
                                    const Project& project,
                                    bool overwrite = false) = 0;
};

}  // namespace chainapi::engine
