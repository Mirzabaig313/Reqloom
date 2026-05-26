// HookTypingsEmitter — writes `chainapi.d.ts` to a project directory
// so hook authors get TypeScript autocomplete on the sandbox `ctx` object.
//
// The typings shape is static today — every project gets the same
// `ChainApiContext` interface. The per-project emitter API is kept
// (rather than shipping a static file) for two reasons:
//   1. The shape will grow per-project once `ctx.actors.<id>.token` is typed.
//   2. Importer-generated projects need to drop the typings file alongside
//      the YAML output, so the writer is the right shape for the consumer.
//
// Same atomic-write pattern as `YamlSchemaWriter`.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>

namespace chainapi::engine {

/// Returns the written `chainapi.d.ts` path on success.
using TypingsEmitResult = std::expected<std::filesystem::path, ChainApiError>;

class HookTypingsEmitter {
public:
    virtual ~HookTypingsEmitter() = default;

    /// Write `<targetDir>/chainapi.d.ts` describing the hook `ctx` surface.
    /// Creates the directory if it does not exist. `overwrite=false` (default)
    /// refuses to clobber an existing file.
    ///
    /// `project` is reserved for future per-project specialisation.
    virtual TypingsEmitResult emit(const std::filesystem::path& targetDir,
                                   const Project& project,
                                   bool overwrite = false) = 0;
};

}  // namespace chainapi::engine
