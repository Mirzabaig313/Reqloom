#pragma once

#include <chainapi/engine/PublicApi.h>

#include <ostream>

namespace chainapi::cli {

/// Machine-readable JSON renderer.
class JsonRenderer {
public:
    explicit JsonRenderer(std::ostream& out);
    void render(const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace chainapi::cli
