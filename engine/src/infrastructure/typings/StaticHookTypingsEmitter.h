// StaticHookTypingsEmitter — `HookTypingsEmitter` that writes a fixed `reqloom.d.ts` body.
#pragma once

#include "HookTypingsEmitter.h"

namespace reqloom::engine {

class StaticHookTypingsEmitter final : public HookTypingsEmitter {
public:
    StaticHookTypingsEmitter();
    StaticHookTypingsEmitter(const StaticHookTypingsEmitter&) = delete;
    StaticHookTypingsEmitter& operator=(const StaticHookTypingsEmitter&) = delete;
    StaticHookTypingsEmitter(StaticHookTypingsEmitter&&) = delete;
    StaticHookTypingsEmitter& operator=(StaticHookTypingsEmitter&&) = delete;
    ~StaticHookTypingsEmitter() override;

    TypingsEmitResult emit(const std::filesystem::path& targetDir,
                           const Project& project,
                           bool overwrite = false) override;
};

}  // namespace reqloom::engine
