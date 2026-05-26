#include "JUnitRenderer.h"

namespace chainapi::cli {

JUnitRenderer::JUnitRenderer(std::ostream& out) : out_(out) {}

void JUnitRenderer::render(const engine::RunResult& /*result*/) {
    out_ << "<testsuites/>\n";
}

}  // namespace chainapi::cli
