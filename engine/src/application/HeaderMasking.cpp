// HeaderMasking — see header.

#include "HeaderMasking.h"

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace chainapi::engine {

namespace {

// Always-redacted header names, lower-cased for case-insensitive match.
// The substring needles below catch the long tail (X-Foo-Token, …).
constexpr std::array<std::string_view, 5> kSensitiveExactNames = {
    "authorization",
    "proxy-authorization",
    "cookie",
    "set-cookie",
    "x-api-key",
};

// Matched as substrings after '-'/'_' are stripped, so "Api-Key" and
// "api_key" both hit "key". "auth" is broad on purpose — over-redacting
// a stray header beats leaking a token.
constexpr std::array<std::string_view, 5> kSensitiveSubstrings = {
    "token",
    "secret",
    "password",
    "key",
    "auth",
};

[[nodiscard]] std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool matchesSubstringPolicy(std::string_view name) {
    const auto lower = toLowerCopy(name);
    std::string flat;
    flat.reserve(lower.size());
    for (char c : lower) {
        if (c != '-' && c != '_') flat.push_back(c);
    }
    for (const auto& needle : kSensitiveSubstrings) {
        if (flat.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

bool isSensitiveHeader(std::string_view name) noexcept {
    const auto lower = toLowerCopy(name);
    for (const auto& exact : kSensitiveExactNames) {
        if (lower == exact) return true;
    }
    return matchesSubstringPolicy(name);
}

bool isSensitiveName(std::string_view name) noexcept {
    return matchesSubstringPolicy(name);
}

std::map<std::string, std::string> maskHeaders(const std::map<std::string, std::string>& headers) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : headers) {
        out[k] = isSensitiveHeader(k) ? std::string{kRedactedHeaderValue} : v;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> maskHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        out.emplace_back(k, isSensitiveHeader(k) ? std::string{kRedactedHeaderValue} : v);
    }
    return out;
}

}  // namespace chainapi::engine
