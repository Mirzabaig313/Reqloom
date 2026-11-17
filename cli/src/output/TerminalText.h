// Text from a project file or an API response, made safe to print on a terminal: control
// characters (ESC and the rest of C0, DEL, C1), Unicode bidirectional and invisible formatting
// characters, and invalid UTF-8 are shown as escapes (`\x1b`, `\u{202e}`) instead of acting on
// the terminal. A project name like "Shop\x1b[2K\rApprove api.example.com" can then neither erase
// nor fake a line of a prompt.
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// `text` with every character that could change how a terminal shows other text replaced by a
/// visible escape. Line breaks are kept only when `keepNewlines` (multi-line details); tabs are
/// kept.
[[nodiscard]] inline std::string terminalSafe(std::string_view text, bool keepNewlines = false) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            if ((byte < 0x20 && byte != '\t' && !(keepNewlines && byte == '\n')) || byte == 0x7F) {
                out += std::format("\\x{:02x}", byte);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            ++i;
            continue;
        }
        const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 0;
        std::uint32_t cp = length == 4 ? byte & 0x07U : length == 3 ? byte & 0x0FU : byte & 0x1FU;
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            valid = (next & 0xC0U) == 0x80U;
            cp = (cp << 6U) | (next & 0x3FU);
        }
        if (!valid) {
            out += std::format("\\x{:02x}", byte);
            ++i;
            continue;
        }
        const bool hidden = (cp >= 0x80 && cp <= 0x9F) || (cp >= 0x200B && cp <= 0x200F) ||
                            (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069) ||
                            cp == 0xFEFF;
        if (hidden) {
            out += std::format("\\u{{{:04x}}}", cp);
        } else {
            out.append(text.substr(i, length));
        }
        i += length;
    }
    return out;
}

}  // namespace reqloom::cli
