// Type-checked JSON field accessors shared by the JSON importers.
//
// `nlohmann::json::value(key, default)` throws `type_error` when the key is
// present with a different type (e.g. `"disabled": null` or `"disabled":
// "true"`). Import input is untrusted, so these helpers fall back to the
// default instead of throwing.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace reqloom::engine::import_json {

/// `obj[key]` when `obj` is an object and the value is a boolean; `def`
/// otherwise (missing key, non-object, or a non-boolean value).
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

/// `obj[key]` when `obj` is an object and the value is a string; `def`
/// otherwise.
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
