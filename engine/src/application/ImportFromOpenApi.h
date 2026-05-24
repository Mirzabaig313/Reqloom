#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>

#include <expected>
#include <filesystem>
#include <string>

namespace chainapi::engine {

class ImportFromOpenApi {
public:
    struct Outcome {
        Project project;
        std::string warnings;
    };

    std::expected<Outcome, ChainApiError> run(const std::filesystem::path& spec) const;
};

}  // namespace chainapi::engine
