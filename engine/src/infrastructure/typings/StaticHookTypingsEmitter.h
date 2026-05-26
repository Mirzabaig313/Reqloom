// StaticHookTypingsEmitter — concrete `HookTypingsEmitter` that writes a
// fixed `chainapi.d.ts` body. When per-project specialisation lands, this
// class either grows the logic or gets joined by a sibling.
#pragma once

#include "HookTypingsEmitter.h"

namespace chainapi::engine {

class StaticHookTypingsEmitter final : public HookTypingsEmitter {
public:
    StaticHookTypingsEmitter();
    ~StaticHookTypingsEmitter() override;

    TypingsEmitResult emit(const std::filesystem::path& targetDir,
                           const Project& project,
                           bool overwrite = false) override;
};

}  // namespace chainapi::engine
