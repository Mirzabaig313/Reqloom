#pragma once

#include <chainapi/engine/PublicApi.h>

#include <ostream>

namespace chainapi::cli {

/// JUnit XML renderer for CI integration.
class JUnitRenderer {
public:
    explicit JUnitRenderer(std::ostream& out);
    void render(const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace chainapi::cli
