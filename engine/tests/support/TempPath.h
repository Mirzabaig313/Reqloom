// Portable unique temp-path helpers for tests. Replaces the POSIX-only
// ::getpid() idiom that fails to compile on MSVC (no <unistd.h>).
#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

namespace chainapi::tests {

/// A process-unique token, stable for the life of the process. Seeded from
/// std::random_device so concurrent test processes don't collide, without
/// reaching for getpid() (which has no portable Windows spelling).
[[nodiscard]] inline std::string processToken() {
    static const std::string token = [] {
        std::random_device rd;
        return std::to_string(rd());
    }();
    return token;
}

/// Build a unique path under the system temp directory. `prefix` names the
/// component (e.g. "chainapi-history"); `suffix` is appended verbatim, so a
/// caller wanting an extension passes ".sqlite". Uniqueness comes from the
/// process token plus a monotonic counter, so repeated calls within one
/// process never alias.
[[nodiscard]] inline std::filesystem::path
uniqueTempPath(std::string_view prefix, std::string_view suffix = "") {
    static std::atomic<unsigned long long> counter{0};
    std::string name;
    name.append(prefix);
    name.push_back('-');
    name.append(processToken());
    name.push_back('-');
    name.append(std::to_string(counter.fetch_add(1)));
    name.append(suffix);
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace chainapi::tests
