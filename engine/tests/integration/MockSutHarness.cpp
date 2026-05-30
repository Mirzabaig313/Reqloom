#include "MockSutHarness.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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

#else  // _WIN32

/// Read the child's stdout pipe up to the "PORT: <n>\n" marker line, the
/// Win32 analogue of readPortFromPipe above. ReadFile blocks until the child
/// writes or the write handle closes, so the 5s wall-clock deadline guards
/// against a child that never reports a port.
int readPortFromPipe(HANDLE readHandle) {
    using namespace std::chrono;
    std::string buffer;
    buffer.reserve(64);
    auto deadline = steady_clock::now() + seconds(5);

    while (steady_clock::now() < deadline) {
        char chunk[64];
        DWORD read = 0;
        if (ReadFile(readHandle, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
            buffer.append(chunk, read);
            auto newline = buffer.find('\n');
            if (newline != std::string::npos) {
                auto line = buffer.substr(0, newline);
                if (auto pos = line.find("PORT:"); pos != std::string::npos) {
                    return std::stoi(line.substr(pos + 5));
                }
            }
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
        ::execl(
            mockPath, mockPath, "--routes", routesFile.c_str(), static_cast<const char*>(nullptr));
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

MockSutHarness::MockSutHarness(const std::filesystem::path& routesFile) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;  // child must inherit the pipe's write end
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &sa, 0)) {
        throw std::runtime_error("mock-sut harness: CreatePipe() failed");
    }
    // The read end stays in this process only — don't let the child inherit
    // it, or the pipe never reports EOF when the child exits.
    SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

    // Build the command line as: "<exe>" --routes "<routesFile>"
    // CreateProcessW needs a mutable wide buffer; quote both paths so spaces
    // in the build directory don't split arguments.
    std::wstring cmd = L"\"";
    cmd += std::filesystem::path(CHAINAPI_MOCK_SUT_PATH).wstring();
    cmd += L"\" --routes \"";
    cmd += routesFile.wstring();
    cmd += L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeHandle;
    si.hStdError = writeHandle;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr,
                                   cmdBuf.data(),
                                   nullptr,
                                   nullptr,
                                   TRUE,  // inherit handles (the pipe write end)
                                   0,
                                   nullptr,
                                   nullptr,
                                   &si,
                                   &pi);
    // The parent never writes to the child's stdout; close our copy of the
    // write end so the pipe reports EOF once the child exits.
    CloseHandle(writeHandle);

    if (!ok) {
        CloseHandle(readHandle);
        throw std::runtime_error("mock-sut harness: CreateProcessW() failed");
    }
    CloseHandle(pi.hThread);
    processHandle_ = pi.hProcess;

    try {
        port_ = readPortFromPipe(readHandle);
    } catch (...) {
        CloseHandle(readHandle);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        processHandle_ = nullptr;
        throw;
    }
    CloseHandle(readHandle);

    baseUrl_ = "http://127.0.0.1:" + std::to_string(port_);

    // Give the server a moment to start listening before tests fire requests.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

MockSutHarness::~MockSutHarness() {
    if (processHandle_ != nullptr) {
        TerminateProcess(processHandle_, 0);
        WaitForSingleObject(processHandle_, 5000);
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
    }
}

#endif

}  // namespace chainapi::tests
