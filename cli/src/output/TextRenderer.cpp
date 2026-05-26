#include "TextRenderer.h"

namespace chainapi::cli {

TextRenderer::TextRenderer(std::ostream& out) : out_(out) {}

void TextRenderer::onEvent(const engine::RunEvent& /*event*/) {}

void TextRenderer::render(const engine::RunResult& /*result*/) {}

}  // namespace chainapi::cli
