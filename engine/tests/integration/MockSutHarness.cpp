#include "MockSutHarness.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace chainapi::tests {

namespace {

#ifndef _WIN32

/// Read the spawned mock-sut's stdout up to the "PORT: <n>\n" marker line.
/// Returns the parsed port, or throws on failure.
int readPortFromPipe(int readFd) {
    using namespace std::chrono;
    std::string buffer;
    buffer.reserve(64);
    auto deadline = steady_clock::now() + seconds(5);

    while (steady_clock::now() < deadline) {
        char chunk[64];
        auto n = ::read(readFd, chunk, sizeof(chunk));
        if (n > 0) {
            buffer.append(chunk, static_cast<std::size_t>(n));
            auto newline = buffer.find('\n');
            if (newline != std::string::npos) {
                auto line = buffer.substr(0, newline);
                if (auto pos = line.find("PORT:"); pos != std::string::npos) {
                    return std::stoi(line.substr(pos + 5));
                }
            }
        } else if (n == 0) {
            std::this_thread::sleep_for(milliseconds(50));
        } else {
            std::this_thread::sleep_for(milliseconds(50));
        }
    }
    throw std::runtime_error("mock-sut: port not reported within 5s");
}

#endif

}  // namespace

#ifndef _WIN32

MockSutHarness::MockSutHarness(const std::filesystem::path& routesFile) {
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) {
        throw std::runtime_error("mock-sut harness: pipe() failed");
    }

    auto pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        throw std::runtime_error("mock-sut harness: fork() failed");
    }

    if (pid == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::close(pipeFds[1]);

        const char* mockPath = CHAINAPI_MOCK_SUT_PATH;
        ::execl(mockPath, mockPath,
                "--routes", routesFile.c_str(),
                static_cast<const char*>(nullptr));
        std::_Exit(127);
    }

    ::close(pipeFds[1]);
    pid_ = static_cast<int>(pid);
    port_ = readPortFromPipe(pipeFds[0]);
    ::close(pipeFds[0]);

    baseUrl_ = "http://127.0.0.1:" + std::to_string(port_);

    // Give the server a moment to start listening before tests fire requests.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

MockSutHarness::~MockSutHarness() {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
}

#else  // _WIN32

MockSutHarness::MockSutHarness(const std::filesystem::path& /*routesFile*/) {
    throw std::runtime_error(
        "mock-sut harness: Windows support not yet implemented");
}

MockSutHarness::~MockSutHarness() = default;

#endif

}  // namespace chainapi::tests
