// JsonExtraction — see header. Implementation moved verbatim from
// ExecutionEngine.cpp so behaviour is identical 
// state (same error codes, same JSONPath subset, same edge cases).
#include "JsonExtraction.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <sstream>
#include <string>
#include <utility>

namespace chainapi::engine {

namespace {
using json = nlohmann::json;
}

std::expected<std::map<std::string, std::string>, ChainApiError>
extractFromJson(const std::string& body,
                const std::vector<Extraction>& extractions) {
    if (extractions.empty()) return std::map<std::string, std::string>{};

    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected(ChainApiError{
            ErrorCode::ResponseParse,
            ErrorClass::Extraction,
            std::string("response is not valid JSON: ") + e.what()});
    }

    // Walk a single segment that may be either "name", "name[N]", or "[N]".
    // Returns nullptr on miss; otherwise the new current pointer.
    auto walkSegment = [](const json* current,
                          const std::string& segment) -> const json* {
        if (segment.empty()) return current;

        const auto bracketPos = segment.find('[');
        const std::string name = (bracketPos == std::string::npos)
            ? segment
            : segment.substr(0, bracketPos);

        if (!name.empty()) {
            if (!current->is_object()) return nullptr;
            auto it = current->find(name);
            if (it == current->end()) return nullptr;
            current = &(*it);
        }

        // Apply each [N] index in turn. There can be multiple, e.g. data[0][1].
        std::size_t pos = bracketPos;
        while (pos != std::string::npos) {
            const auto closePos = segment.find(']', pos);
            if (closePos == std::string::npos) return nullptr;
            const auto indexStr = segment.substr(pos + 1, closePos - pos - 1);
            std::size_t index = 0;
            const auto* first = indexStr.data();
            const auto* last = first + indexStr.size();
            const auto fc = std::from_chars(first, last, index);
            if (fc.ec != std::errc{}) return nullptr;

            if (!current->is_array() || index >= current->size()) return nullptr;
            current = &(*current)[index];

            pos = segment.find('[', closePos);
        }
        return current;
    };

    std::map<std::string, std::string> values;
    for (const auto& ext : extractions) {
        auto path = ext.sourcePath;
        if (path.starts_with("$.")) path = path.substr(2);

        const json* current = &doc;
        bool found = true;
        std::istringstream ss(path);
        std::string segment;
        while (std::getline(ss, segment, '.')) {
            current = walkSegment(current, segment);
            if (!current) { found = false; break; }
        }
        if (!found) {
            return std::unexpected(ChainApiError{
                ErrorCode::ExtractionFailed,
                ErrorClass::Extraction,
                "extract path '" + ext.sourcePath +
                "' not found in response (variable: " + ext.variableName + ")"});
        }

        std::string value = current->is_string()
            ? current->get<std::string>()
            : current->dump();
        values[ext.variableName] = std::move(value);
    }
    return values;
}

}  // namespace chainapi::engine
