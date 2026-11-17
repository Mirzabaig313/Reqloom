// Points every test executable's per-user config at the build tree before main() (see
// cmake/ReqloomTestIsolation.cmake), so no test writes the user's real config.
//   - The project trust file (approvals, folder bindings, the CLI approval setting) goes through
//     the engine's test-only seam: the shipped programs honour no environment variable for it.
//   - REQLOOM_CONFIG_DIR is still set for the desktop's saved-response store, which reads it.
// A REQLOOM_CONFIG_DIR already set by the caller is kept and used for both.
// Test executables that do not link the engine (QML-only UI tests) have no trust file to move.
#if __has_include(<reqloom/engine/Factories.h>)
#include <reqloom/engine/Factories.h>
#define REQLOOM_TEST_HAS_ENGINE 1
#endif

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

[[maybe_unused]] const bool kConfigDirIsolated = [] {
    constexpr const char* kVar = "REQLOOM_CONFIG_DIR";
    std::string dir{REQLOOM_TEST_CONFIG_DIR};
#if defined(_WIN32)
    char* existing = nullptr;
    std::size_t length{0};
    if (_dupenv_s(&existing, &length, kVar) == 0 && existing != nullptr && *existing != '\0') {
        dir = existing;
    } else {
        _putenv_s(kVar, REQLOOM_TEST_CONFIG_DIR);
    }
    std::free(existing);
#else
    if (const char* existing = std::getenv(kVar); existing != nullptr && *existing != '\0') {
        dir = existing;
    } else {
        setenv(kVar, REQLOOM_TEST_CONFIG_DIR, 1);
    }
#endif
#if defined(REQLOOM_TEST_HAS_ENGINE)
    reqloom::engine::setConfigDirectoryForTesting(std::filesystem::path{dir});
#endif
    return true;
}();

}  // namespace
