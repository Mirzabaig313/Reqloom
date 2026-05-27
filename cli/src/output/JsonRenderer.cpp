#include "JsonRenderer.h"

namespace chainapi::cli {

JsonRenderer::JsonRenderer(std::ostream& out) : out_(out) {}

void JsonRenderer::render(const engine::RunResult& /*result*/) {
    out_ << "{}\n";
}

}  // namespace chainapi::cli
