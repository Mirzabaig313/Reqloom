// Cookies — see header.

#include "Cookies.h"

#include <cctype>
#include <string>

namespace chainapi::engine::cookies {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char const c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

std::optional<std::pair<std::string, std::string>> parseSetCookie(std::string_view header) {
    // Cookie definition is up to the first `;`; everything after is
    // attributes (Path, Domain, Expires, Max-Age, Secure, HttpOnly,
    // SameSite). We don't honour any of them — see Cookies.h.
    auto semi = header.find(';');
    auto cookie = (semi == std::string_view::npos) ? header : header.substr(0, semi);

    auto eq = cookie.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }

    auto name = trim(cookie.substr(0, eq));
    auto value = trim(cookie.substr(eq + 1));
    if (name.empty()) {
        return std::nullopt;
    }

    return std::make_pair(std::string{name}, std::string{value});
}

std::vector<std::pair<std::string, std::string>> collectFromResponse(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        if (toLowerCopy(k) != "set-cookie") {
            continue;
        }
        if (auto parsed = parseSetCookie(v); parsed) {
            out.push_back(std::move(*parsed));
        }
    }
    return out;
}

std::string formatRequestHeader(const std::map<std::string, std::string>& jar) {
    std::string out;
    for (const auto& [name, value] : jar) {
        if (!out.empty()) {
            out += "; ";
        }
        out += name;
        out += "=";
        out += value;
    }
    return out;
}

}  // namespace chainapi::engine::cookies
