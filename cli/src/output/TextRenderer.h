#pragma once

#include <chainapi/engine/PublicApi.h>

#include <ostream>

namespace chainapi::cli {

/// Human-readable run renderer. Default for `chainapi run`.
class TextRenderer {
public:
    explicit TextRenderer(std::ostream& out);

    void onEvent(const engine::RunEvent& event);
    void render(const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace chainapi::cli
