// HookTypingsEmitter — writes `reqloom.d.ts` so hook authors get TypeScript autocomplete on the
// sandbox `ctx`. Static today; the per-project API lets importer-generated projects drop it beside
// the YAML. Same atomic-write pattern as `YamlSchemaWriter`.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>

namespace reqloom::engine {

/// Returns the written `reqloom.d.ts` path on success.
using TypingsEmitResult = std::expected<std::filesystem::path, ReqloomError>;

class HookTypingsEmitter {
public:
    HookTypingsEmitter() = default;
    HookTypingsEmitter(const HookTypingsEmitter&) = delete;
    HookTypingsEmitter& operator=(const HookTypingsEmitter&) = delete;
    HookTypingsEmitter(HookTypingsEmitter&&) = delete;
    HookTypingsEmitter& operator=(HookTypingsEmitter&&) = delete;
    virtual ~HookTypingsEmitter() = default;

    /// Write `<targetDir>/reqloom.d.ts` describing the hook `ctx` surface.
    /// Creates the directory if it does not exist. `overwrite=false` (default)
    /// refuses to clobber an existing file.
    ///
    /// `project` is reserved for future per-project specialisation.
    virtual TypingsEmitResult emit(const std::filesystem::path& targetDir,
                                   const Project& project,
                                   bool overwrite = false) = 0;
};

}  // namespace reqloom::engine
