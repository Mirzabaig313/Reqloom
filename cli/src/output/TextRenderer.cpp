#include "TextRenderer.h"

namespace chainapi::cli {

TextRenderer::TextRenderer(std::ostream& out) : out_(out) {}

void TextRenderer::onEvent(const engine::RunEvent& /*event*/) {
    // Phase 1: render each step inline as it lands.
}

void TextRenderer::render(const engine::RunResult& /*result*/) {
    // Phase 1: print summary + chain timing.
}

}  // namespace chainapi::cli
