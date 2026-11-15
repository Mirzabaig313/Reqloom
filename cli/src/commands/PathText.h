// Conversions between UTF-8 command-line text and filesystem paths.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// A path from UTF-8 text. Going through `char8_t` keeps non-ASCII names intact on Windows, where
/// a `std::string` would be read in the ANSI code page.
[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

/// A path as UTF-8 text for messages and reports.
[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string{text.begin(), text.end()};
}

}  // namespace reqloom::cli
