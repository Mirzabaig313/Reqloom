// Codecs — see header.
#include "Codecs.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace chainapi::engine::codecs {

// ─── base64 ─────────────────────────────────────────────────────────────────

namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::uint8_t, 256> makeBase64DecodeTable() {
    std::array<std::uint8_t, 256> t{};
    for (auto& v : t) v = 0xFF;
    for (std::uint8_t i = 0; i < kBase64Alphabet.size(); ++i) {
        t[static_cast<std::uint8_t>(kBase64Alphabet[i])] = i;
    }
    return t;
}

}  // namespace

std::string base64Encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
        const auto b2 = static_cast<std::uint8_t>(input[i + 2]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(kBase64Alphabet[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        out.push_back(kBase64Alphabet[b2 & 0x3F]);
        i += 3;
    }
    if (i < input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        if (i + 1 == input.size()) {
            out.push_back(kBase64Alphabet[(b0 << 4) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else {
            const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
            out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
            out.push_back(kBase64Alphabet[(b1 << 2) & 0x3F]);
            out.push_back('=');
        }
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view input) {
    static constexpr auto kTable = makeBase64DecodeTable();

    std::string buf;
    buf.reserve(input.size());
    int padding = 0;
    for (const char c : input) {
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) return std::nullopt;
        if (kTable[static_cast<std::uint8_t>(c)] == 0xFF) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return std::nullopt;
        }
        buf.push_back(c);
    }
    if (padding > 2) return std::nullopt;

    std::string out;
    out.reserve((buf.size() * 3) / 4);

    std::size_t i = 0;
    while (i + 4 <= buf.size()) {
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        const auto v2 = kTable[static_cast<std::uint8_t>(buf[i + 2])];
        const auto v3 = kTable[static_cast<std::uint8_t>(buf[i + 3])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
        out.push_back(static_cast<char>((v2 << 6) | v3));
        i += 4;
    }
    const auto rem = buf.size() - i;
    if (rem == 0) return out;
    if (rem == 1) return std::nullopt;
    if (rem == 2) {
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
    } else {
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        const auto v2 = kTable[static_cast<std::uint8_t>(buf[i + 2])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
    }
    return out;
}

// ─── hex ────────────────────────────────────────────────────────────────────

std::string hexEncode(std::string_view input) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (const char c : input) {
        const auto b = static_cast<std::uint8_t>(c);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::optional<std::string> hexDecode(std::string_view input) {
    if (input.size() % 2 != 0) return std::nullopt;

    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const int hi = value(input[i]);
        const int lo = value(input[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// ─── url ────────────────────────────────────────────────────────────────────

std::string urlEncode(std::string_view input) {
    constexpr char kHexUpper[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (const char rawChar : input) {
        const auto c = static_cast<unsigned char>(rawChar);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHexUpper[c >> 4]);
            out.push_back(kHexUpper[c & 0x0F]);
        }
    }
    return out;
}

std::optional<std::string> urlDecode(std::string_view input) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%') {
            if (i + 2 >= input.size()) return std::nullopt;
            const int hi = value(input[i + 1]);
            const int lo = value(input[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace chainapi::engine::codecs
