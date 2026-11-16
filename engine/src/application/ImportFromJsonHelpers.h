// Type-checked JSON field accessors shared by the JSON importers.
//
// `json::value(key, default)` throws `type_error` when the key has a different type
// (`"disabled": null`); import input is untrusted, so these helpers return the default.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace reqloom::engine::import_json {

/// `obj[key]` when it is a boolean, else `def`.
[[nodiscard]] inline bool boolOr(const nlohmann::json& obj, std::string_view key, bool def) {
    if (!obj.is_object()) {
        return def;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return def;
    }
    return it->get<bool>();
}

/// `obj[key]` when it is a string, else `def`.
[[nodiscard]] inline std::string stringOr(const nlohmann::json& obj,
                                          std::string_view key,
                                          std::string def) {
    if (!obj.is_object()) {
        return def;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return def;
    }
    return it->get<std::string>();
}

}  // namespace reqloom::engine::import_json
