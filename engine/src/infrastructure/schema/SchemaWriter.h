// Engine-internal interface for writing a Project back to a chainapi.yaml
// (or layered set of files). Concrete impl: YamlSchemaWriter (yaml-cpp).
//
// Importers are the primary consumers — after the AI / OpenAPI / Postman
// importer produces a Project value, the writer persists it to a directory
// the user picked. Provenance metadata round-trips intact.
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

    /// Write `project` into a directory rooted at `targetDir`. Creates the
    /// directory if it does not exist, refuses to overwrite existing files
    /// unless `overwrite` is set, and returns the path to the produced
    /// `chainapi.yaml`.
    ///
    /// Layout produced (matches what the parser expects):
    ///   targetDir/chainapi.yaml
    ///   targetDir/actors/<id>.yaml
    ///   targetDir/resources/<id>.yaml
    ///   targetDir/environments/<n>.yaml
    ///
    /// Implementations must round-trip: writing then re-parsing the same
    /// project yields a structurally equivalent Project (modulo map ordering).
    virtual SchemaWriteResult write(const std::filesystem::path& targetDir,
                                    const Project& project,
                                    bool overwrite = false) = 0;
};

}  // namespace chainapi::engine
