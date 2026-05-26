// ImportFromOpenApi — direct (non-LLM) parser that produces a Project from
// an OpenAPI 3.x document.
#include "ImportFromOpenApi.h"

namespace chainapi::engine {

std::expected<ImportFromOpenApi::Outcome, ChainApiError> ImportFromOpenApi::run(
    const std::filesystem::path& /*spec*/) const {
    return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                         ErrorClass::Schema,
                                         "OpenAPI import not yet implemented."});
}

}  // namespace chainapi::engine
