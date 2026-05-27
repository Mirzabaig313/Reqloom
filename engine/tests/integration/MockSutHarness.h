// MockSutHarness — spawns the mock-sut binary, captures its port, and
// shuts it down cleanly when the test fixture is destroyed.
//
// Each test instantiates a `MockSutHarness` with a routes JSON file. The
// `baseUrl()` accessor returns "http://127.0.0.1:<port>" suitable for
// injection into a chainapi project's environment.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace chainapi::tests {

class MockSutHarness {
public:
    /// Spawn the mock-sut process with the given routes file. Throws on
    /// failure. The harness picks a free port automatically.
    explicit MockSutHarness(const std::filesystem::path& routesFile);

    ~MockSutHarness();

    MockSutHarness(const MockSutHarness&) = delete;
    MockSutHarness& operator=(const MockSutHarness&) = delete;

    /// Base URL the spawned server is listening on, e.g. "http://127.0.0.1:53921".
    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

    [[nodiscard]] int port() const noexcept { return port_; }

private:
    int port_{0};
    std::string baseUrl_;
#ifdef _WIN32
    void* processHandle_{nullptr};
#else
    int pid_{0};
#endif
};

}  // namespace chainapi::tests
