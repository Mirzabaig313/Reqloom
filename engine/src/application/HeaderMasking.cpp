// HeaderMasking — see header.

#include "HeaderMasking.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace chainapi::engine {

namespace {

// Exact-match allowlist of always-redacted header names. Case-insensitive.
// Kept short and explicit so the policy is auditable; the substring rules
// below catch the long tail (X-Foo-Token, X-Bar-Secret, etc.).
constexpr std::array<std::string_view, 5> kSensitiveExactNames = {
    "authorization",
    "proxy-authorization",
    "cookie",
    "set-cookie",
    "x-api-key",
};

// Substring needles. Any header name that contains one of these
// (case-insensitive) is redacted. Catches the long tail of vendor-
// specific names like X-Stripe-Secret-Key, X-Auth-Token, X-Refresh-Token.
constexpr std::array<std::string_view, 5> kSensitiveSubstrings = {
    "token",
    "secret",
    "password",
    "apikey",  // covers ApiKey, API-Key after dash strip
    "auth",    // matches auth, authn, refresh-auth — accepted false-positive rate
};

[[nodiscard]] std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

bool isSensitiveHeader(std::string_view name) noexcept {
    const auto lower = toLowerCopy(name);

    for (const auto& exact : kSensitiveExactNames) {
        if (lower == exact) return true;
    }

    // Substring scan against a name with dashes stripped — so "Api-Key"
    // matches the "apikey" needle. We intentionally don't strip from
    // the input form returned to the user; redaction policy is
    // matching-only.
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
