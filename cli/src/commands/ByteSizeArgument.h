// Size arguments on the command line (`--failure-body-size 16KiB`).
#pragma once

#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace reqloom::cli {

/// A byte count (`4096`), or a whole number followed by `KiB` or `MiB` (`16KiB`, `1MiB`).
/// nullopt for anything else, including negative numbers, other units and overflow.
[[nodiscard]] inline std::optional<std::size_t> parseByteSizeArgument(std::string_view text) {
    std::size_t number{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, number);
    if (ec != std::errc{} || ptr == text.data()) {
        return std::nullopt;
    }
    const std::string_view unit{ptr, static_cast<std::size_t>(end - ptr)};
    std::size_t multiplier{1};
    if (unit == "KiB") {
        multiplier = 1024;
    } else if (unit == "MiB") {
        multiplier = std::size_t{1024} * 1024;
    } else if (!unit.empty()) {
        return std::nullopt;
    }
    if (number > std::numeric_limits<std::size_t>::max() / multiplier) {
        return std::nullopt;
    }
    return number * multiplier;
}

}  // namespace reqloom::cli
