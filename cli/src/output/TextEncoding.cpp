// Text encoding for machine-readable reports: valid UTF-8 for JSON, valid XML 1.0 for JUnit.

#include "TextEncoding.h"

#include <cstdint>

namespace reqloom::cli {

namespace {

constexpr std::string_view kReplacementUtf8 = "\xEF\xBF\xBD";
constexpr char32_t kReplacement = 0xFFFD;

[[nodiscard]] constexpr unsigned char byteAt(std::string_view s, std::size_t i) noexcept {
    return static_cast<unsigned char>(s[i]);
}

/// Length of the well-formed UTF-8 sequence at the start of `s` (Unicode table 3-7), 0 if none.
[[nodiscard]] std::size_t sequenceLength(std::string_view s) noexcept {
    const unsigned char lead = byteAt(s, 0);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length{0};
    unsigned char low{0x80};
    unsigned char high{0xBF};
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead == 0xE0) {
        length = 3;
        low = 0xA0;
    } else if (lead == 0xED) {
        length = 3;
        high = 0x9F;
    } else if (lead >= 0xE1 && lead <= 0xEF) {
        length = 3;
    } else if (lead == 0xF0) {
        length = 4;
        low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        length = 4;
    } else if (lead == 0xF4) {
        length = 4;
        high = 0x8F;
    } else {
        return 0;
    }
    if (s.size() < length || byteAt(s, 1) < low || byteAt(s, 1) > high) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        if ((byteAt(s, i) & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return length;
}

[[nodiscard]] char32_t decode(std::string_view sequence) noexcept {
    const unsigned char lead = byteAt(sequence, 0);
    if (sequence.size() == 1) {
        return lead;
    }
    const unsigned leadBits = sequence.size() == 2 ? 0x1FU : sequence.size() == 3 ? 0x0FU : 0x07U;
    std::uint32_t cp = lead & leadBits;
    for (std::size_t i = 1; i < sequence.size(); ++i) {
        cp = (cp << 6U) | (byteAt(sequence, i) & 0x3FU);
    }
    return static_cast<char32_t>(cp);
}

/// Calls `emit(bytes, codePoint)` for each character, with U+FFFD for each invalid byte.
template <typename Emit>
void forEachCodePoint(std::string_view input, Emit&& emit) {
    std::size_t i = 0;
    while (i < input.size()) {
        const auto rest = input.substr(i);
        const std::size_t length = sequenceLength(rest);
        if (length == 0) {
            emit(kReplacementUtf8, kReplacement);
            ++i;
        } else {
            const auto sequence = rest.substr(0, length);
            emit(sequence, decode(sequence));
            i += length;
        }
    }
}

[[nodiscard]] constexpr bool xmlAllowed(char32_t cp) noexcept {
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

[[nodiscard]] std::string xmlEscape(std::string_view input, bool attribute) {
    std::string out;
    out.reserve(input.size());
    forEachCodePoint(input, [&](std::string_view bytes, char32_t cp) {
        if (!xmlAllowed(cp)) {
            out += kReplacementUtf8;
        } else if (cp == U'<') {
            out += "&lt;";
        } else if (cp == U'>') {
            out += "&gt;";
        } else if (cp == U'&') {
            out += "&amp;";
        } else if (attribute && cp == U'"') {
            out += "&quot;";
        } else if (attribute && cp == U'\'') {
            out += "&apos;";
        } else if (attribute && cp == U'\t') {
            out += "&#9;";
        } else if (attribute && cp == U'\n') {
            out += "&#10;";
        } else if (attribute && cp == U'\r') {
            out += "&#13;";
        } else {
            out += bytes;
        }
    });
    return out;
}

}  // namespace

std::string toValidUtf8(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    forEachCodePoint(input, [&](std::string_view bytes, char32_t) { out += bytes; });
    return out;
}

std::string jsonEscape(std::string_view input) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() + 2);
    forEachCodePoint(input, [&](std::string_view bytes, char32_t cp) {
        switch (cp) {
            case U'"':
                out += "\\\"";
                break;
            case U'\\':
                out += "\\\\";
                break;
            case U'\b':
                out += "\\b";
                break;
            case U'\f':
                out += "\\f";
                break;
            case U'\n':
                out += "\\n";
                break;
            case U'\r':
                out += "\\r";
                break;
            case U'\t':
                out += "\\t";
                break;
            default:
                if (cp < 0x20) {
                    out += "\\u00";
                    out += kHex[cp >> 4U];
                    out += kHex[cp & 0xFU];
                } else {
                    out += bytes;
                }
                break;
        }
    });
    return out;
}

std::string xmlText(std::string_view input) {
    return xmlEscape(input, /*attribute=*/false);
}

std::string xmlAttribute(std::string_view input) {
    return xmlEscape(input, /*attribute=*/true);
}

std::string xmlCdata(std::string_view input) {
    constexpr std::string_view kTerminator = "]]>";
    std::string out = "<![CDATA[";
    out.reserve(input.size() + 12);
    forEachCodePoint(input, [&](std::string_view bytes, char32_t cp) {
        out += xmlAllowed(cp) ? bytes : kReplacementUtf8;
        if (cp == U'>' && out.ends_with(kTerminator)) {
            out.insert(out.size() - 1, "]]><![CDATA[");
        }
    });
    out += kTerminator;
    return out;
}

std::string firstLine(std::string_view text, std::size_t maxBytes) {
    auto line = text.substr(0, text.find_first_of("\r\n"));
    if (line.size() <= maxBytes) {
        return std::string{line};
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (byteAt(line, cut) & 0xC0U) == 0x80U) {
        --cut;
    }
    return std::string{line.substr(0, cut)} + "\xE2\x80\xA6";
}

}  // namespace reqloom::cli
