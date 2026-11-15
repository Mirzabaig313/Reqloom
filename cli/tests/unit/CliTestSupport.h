// Shared helpers for CLI command tests: scratch directories and captured stdio.
#pragma once

#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace reqloom::cli::tests {

/// A self-cleaning scratch directory under the system temp path.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const unsigned seq = counter.fetch_add(1, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("reqloom-cli-test-" + std::to_string(static_cast<long long>(stamp)) + "-" +
                 std::to_string(seq));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path write(const std::string& name, const std::string& body) const {
        const auto full = path_ / name;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out{full, std::ios::binary};
        out << body;
        return full;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline std::string readText(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] inline QStringList argList(const std::vector<std::string>& parts) {
    QStringList args;
    for (const auto& part : parts) {
        args.append(QString::fromStdString(part));
    }
    return args;
}

struct CapturedRun {
    int rc{0};
    std::string out;
    std::string err;
};

/// Runs `command` with stdout and stderr captured.
[[nodiscard]] inline CapturedRun captureRun(const std::function<int()>& command) {
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    CapturedRun c;
    c.rc = command();
    std::fflush(stdout);
    std::fflush(stderr);
    c.out = testing::internal::GetCapturedStdout();
    c.err = testing::internal::GetCapturedStderr();
    return c;
}

}  // namespace reqloom::cli::tests
