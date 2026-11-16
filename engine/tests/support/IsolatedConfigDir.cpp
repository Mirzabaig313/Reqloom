// Points REQLOOM_CONFIG_DIR at the build tree before main() in every test
// executable (see cmake/ReqloomTestIsolation.cmake). A value already set by
// the caller is kept.
#include <cstdlib>

namespace {

[[maybe_unused]] const bool kConfigDirIsolated = [] {
    constexpr const char* kVar = "REQLOOM_CONFIG_DIR";
#if defined(_WIN32)
    std::size_t length{0};
    if (getenv_s(&length, nullptr, 0, kVar) == 0 && length > 1) {
        return true;
    }
    _putenv_s(kVar, REQLOOM_TEST_CONFIG_DIR);
#else
    if (const char* existing = std::getenv(kVar); existing != nullptr && *existing != '\0') {
        return true;
    }
    setenv(kVar, REQLOOM_TEST_CONFIG_DIR, 1);
#endif
    return true;
}();

}  // namespace
