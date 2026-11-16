// Writes a finished report to `--output` or standard output, reporting every write failure.
#pragma once

#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace reqloom::cli {

/// Replace `path` with `document`: written to a temporary file in the same directory, flushed,
/// then renamed over `path`. On any failure `path` is left as it was.
/// @return A one-line message naming the file on failure.
[[nodiscard]] std::expected<void, std::string> writeReportFile(const std::filesystem::path& path,
                                                               std::string_view document);

/// Write `document` to `stream` and flush it; fails if this or any earlier write to `stream`
/// failed.
[[nodiscard]] std::expected<void, std::string> writeReportStream(std::FILE* stream,
                                                                 std::string_view document);

}  // namespace reqloom::cli
