// Portable time helpers for tests. POSIX timegm() has no standard C++
// equivalent; MSVC spells it _mkgmtime. Mirrors the guard already used in
// engine/src/infrastructure/storage/SqliteHistoryStore.cpp.
#pragma once

#include <ctime>

namespace chainapi::tests {

/// Convert a UTC std::tm to time_t without consulting the local timezone,
/// the way POSIX timegm() does. On Windows the equivalent is _mkgmtime.
[[nodiscard]] inline std::time_t timegmUtc(std::tm* tm) {
#if defined(_WIN32)
    return _mkgmtime(tm);
#else
    return ::timegm(tm);
#endif
}

}  // namespace chainapi::tests
