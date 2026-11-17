// HeaderMasking — see header.

#include "HeaderMasking.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace reqloom::engine {

namespace {

// Always-redacted header names, lower-cased; the substring needles below catch the long tail.
constexpr std::array<std::string_view, 5> kSensitiveExactNames = {
    "authorization",
    "proxy-authorization",
    "cookie",
    "set-cookie",
    "x-api-key",
};

// Matched as substrings after '-'/'_' are stripped; "auth" is broad on purpose (over-redacting
// beats leaking).
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
    for (const char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Lower case with `-` and `_` removed.
[[nodiscard]] std::string flatLower(std::string_view name) {
    std::string flat;
    flat.reserve(name.size());
    for (const char c : name) {
        if (c != '-' && c != '_') {
            flat.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return flat;
}

[[nodiscard]] bool matchesSubstringPolicy(std::string_view name) {
    const auto lower = toLowerCopy(name);
    std::string flat;
    flat.reserve(lower.size());
    for (const char c : lower) {
        if (c != '-' && c != '_') {
            flat.push_back(c);
        }
    }
    return std::ranges::any_of(kSensitiveSubstrings, [&](const auto& needle) {
        return flat.find(needle) != std::string::npos;
    });
}

}  // namespace

bool isSensitiveHeader(std::string_view name) noexcept {
    // Fail closed: if classification throws (OOM), treat the header as sensitive; keeps the
    // noexcept contract.
    try {
        const auto lower = toLowerCopy(name);
        for (const auto& exact : kSensitiveExactNames) {
            if (lower == exact) {
                return true;
            }
        }
        return matchesSubstringPolicy(name);
    } catch (...) {
        return true;
    }
}

bool isCredentialName(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 13> kNeedles{"authorization",
                                                               "cookie",
                                                               "token",
                                                               "secret",
                                                               "password",
                                                               "passwd",
                                                               "apikey",
                                                               "accesskey",
                                                               "privatekey",
                                                               "credential",
                                                               "session",
                                                               "bearer",
                                                               "jwt"};
    try {
        const auto flat = flatLower(name);
        return flat == "auth" || flat == "key" ||
               std::ranges::any_of(kNeedles, [&flat](const std::string_view needle) {
                   return flat.find(needle) != std::string::npos;
               });
    } catch (...) {
        return true;
    }
}

bool isCredentialHeader(std::string_view name) noexcept {
    return isSensitiveHeader(name) || isCredentialName(name);
}

bool isCredentialBodyField(std::string_view name) noexcept {
    try {
        const auto flat = flatLower(name);
        if (flat == "key" || flat == "auth" || flat.ends_with("type") || flat.ends_with("hint")) {
            return false;
        }
        return isCredentialName(name);
    } catch (...) {
        return true;
    }
}

bool isAgentSensitiveName(std::string_view name) noexcept {
    return isSensitiveHeader(name) || isCredentialName(name);
}

bool isSensitiveName(std::string_view name) noexcept {
    try {
        return matchesSubstringPolicy(name);
    } catch (...) {
        return true;
    }
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

}  // namespace reqloom::engine
