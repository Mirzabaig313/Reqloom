// SecretRegistry — see header.

#include "SecretRegistry.h"

#include "Codecs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

namespace {

/// `value` as it appears inside a JSON string: `"`, `\` and control bytes escaped.
[[nodiscard]] std::string jsonEscaped(std::string_view value) {
    std::string out{};
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

// Words that are long enough to register but appear in ordinary text; a
// "secret" equal to one of them would shred every event.
constexpr std::array<std::string_view, 4> kCommonWords = {
    "bearer",
    "undefined",
    "<redacted>",
    "***redacted***",
};

[[nodiscard]] std::string asciiLower(std::string_view s) {
    std::string out{};
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] std::string base64Url(std::string b64) {
    std::ranges::replace(b64, '+', '-');
    std::ranges::replace(b64, '/', '_');
    return b64;
}

[[nodiscard]] std::string stripPadding(std::string b64) {
    while (!b64.empty() && b64.back() == '=') {
        b64.pop_back();
    }
    return b64;
}

/// Form encoding (application/x-www-form-urlencoded) spells a space '+'.
[[nodiscard]] std::string formEncode(std::string_view value) {
    std::string encoded = codecs::urlEncode(value);
    std::string out{};
    out.reserve(encoded.size());
    for (std::size_t i{}; i < encoded.size(); ++i) {
        if (encoded.compare(i, 3, "%20") == 0) {
            out.push_back('+');
            i += 2;
        } else {
            out.push_back(encoded[i]);
        }
    }
    return out;
}

}  // namespace

bool SecretRegistry::isRegistrable(std::string_view value) noexcept {
    if (value.size() < kMinSecretLength) {
        return false;
    }
    if (std::ranges::all_of(
            value, [](const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; })) {
        return false;
    }
    try {
        const auto lower = asciiLower(value);
        return std::ranges::find(kCommonWords, std::string_view{lower}) == kCommonWords.end();
    } catch (...) {
        // Allocation failure: registering is the safe side.
        return true;
    }
}

void SecretRegistry::add(std::string_view value) {
    if (!isRegistrable(value)) {
        return;
    }
    const std::unique_lock lock(mutex_);
    addPatternLocked(std::string{value});
    addPatternLocked(codecs::urlEncode(value));
    addPatternLocked(formEncode(value));
    auto b64 = codecs::base64Encode(value);
    addPatternLocked(base64Url(b64));
    addPatternLocked(stripPadding(base64Url(b64)));
    addPatternLocked(stripPadding(b64));
    addPatternLocked(std::move(b64));
    if (jsonEscaped_) {
        addPatternLocked(jsonEscaped(value));
    }
}

std::size_t SecretRegistry::longestPattern() const {
    const std::shared_lock lock(mutex_);
    return longestPattern_;
}

void SecretRegistry::enableJsonEscapedSpellings() {
    const std::unique_lock lock(mutex_);
    if (jsonEscaped_) {
        return;
    }
    jsonEscaped_ = true;
    const std::vector<std::string> known{patterns_.begin(), patterns_.end()};
    for (const auto& pattern : known) {
        addPatternLocked(jsonEscaped(pattern));
    }
}

void SecretRegistry::addPatternLocked(std::string pattern) {
    if (pattern.size() < kMinSecretLength || patterns_.contains(pattern)) {
        return;
    }
    auto& bucket = byFirstByte_.at(static_cast<unsigned char>(pattern.front()));
    const auto pos = std::ranges::find_if(
        bucket, [&](const std::string& existing) { return existing.size() < pattern.size(); });
    bucket.insert(pos, pattern);
    shortestPattern_ =
        shortestPattern_ == 0 ? pattern.size() : std::min(shortestPattern_, pattern.size());
    longestPattern_ = std::max(longestPattern_, pattern.size());
    patterns_.insert(std::move(pattern));
}

bool SecretRegistry::containsAny(std::string_view text) const {
    const std::shared_lock lock(mutex_);
    if (patterns_.empty() || text.size() < shortestPattern_) {
        return false;
    }
    for (std::size_t i{}; i + shortestPattern_ <= text.size(); ++i) {
        for (const auto& pattern : byFirstByte_.at(static_cast<unsigned char>(text[i]))) {
            if (pattern.size() <= text.size() - i &&
                text.compare(i, pattern.size(), pattern) == 0) {
                return true;
            }
        }
    }
    return false;
}

std::string SecretRegistry::redact(std::string_view text) const {
    return redact(text, kRedactedValueMarker);
}

std::string SecretRegistry::redact(std::string_view text, std::string_view marker) const {
    const std::shared_lock lock(mutex_);
    if (patterns_.empty() || text.size() < shortestPattern_) {
        return std::string{text};
    }
    // Mark every byte covered by a match (longest first), then collapse each run into one marker,
    // so overlapping secrets leave no fragment.
    std::vector<bool> covered{};
    std::size_t coverEnd{0};
    for (std::size_t i{}; i + shortestPattern_ <= text.size(); ++i) {
        const auto& bucket = byFirstByte_.at(static_cast<unsigned char>(text[i]));
        for (const auto& pattern : bucket) {
            if (pattern.size() <= text.size() - i &&
                text.compare(i, pattern.size(), pattern) == 0) {
                if (covered.empty()) {
                    covered.assign(text.size(), false);
                }
                const auto end = i + pattern.size();
                for (std::size_t k = std::max(i, coverEnd); k < end; ++k) {
                    covered[k] = true;
                }
                coverEnd = std::max(coverEnd, end);
                break;
            }
        }
    }
    if (covered.empty()) {
        return std::string{text};
    }
    std::string out{};
    out.reserve(text.size());
    for (std::size_t i{}; i < text.size(); ++i) {
        if (!covered[i]) {
            out.push_back(text[i]);
        } else if (i == 0 || !covered[i - 1]) {
            out.append(marker);
        }
    }
    return out;
}

bool SecretRegistry::empty() const {
    const std::shared_lock lock(mutex_);
    return patterns_.empty();
}

}  // namespace reqloom::engine
