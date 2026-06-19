// `reqloom import` command tests — exercise the argument contract and the
// import → writeProject glue end-to-end against a temp spec and output dir.
// The engine's importFromOpenApi/writeProject are covered separately; these
// assert the CLI's exit codes and on-disk effects.

#include "commands/ImportCommand.h"

#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// A self-cleaning scratch directory under the system temp path.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const unsigned seq = counter.fetch_add(1, std::memory_order_relaxed);
        path_ = fs::temp_directory_path() /
                ("reqloom-cli-import-" + std::to_string(static_cast<long long>(stamp)) + "-" +
                 std::to_string(seq));
        fs::create_directories(path_);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    [[nodiscard]] fs::path write(const std::string& name, const std::string& body) const {
        const auto full = path_ / name;
        std::ofstream out{full};
        out << body;
        return full;
    }

private:
    fs::path path_;
};

[[nodiscard]] QStringList toArgs(const std::vector<std::string>& parts) {
    QStringList args;
    for (const auto& part : parts) {
        args.append(QString::fromStdString(part));
    }
    return args;
}

constexpr const char* kMinimalSpec = R"YAML(
openapi: "3.0.3"
info: { title: Widgets, version: "1.0" }
paths:
  /widgets:
    get:
      responses:
        "200":
          description: ok
          content:
            application/json:
              example: { id: "w1" }
)YAML";

}  // namespace

namespace reqloom::cli::tests {

TEST(ImportCommand, missing_spec_argument_returns_usage_error) {
    EXPECT_EQ(importCommand(QStringList{}), 2);
}

TEST(ImportCommand, help_flag_returns_success) {
    EXPECT_EQ(importCommand(toArgs({"--help"})), 0);
}

TEST(ImportCommand, unknown_flag_returns_usage_error) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    EXPECT_EQ(importCommand(toArgs({spec.string(), "--nope"})), 2);
}

TEST(ImportCommand, missing_spec_file_returns_failure) {
    ScratchDir scratch;
    const auto missing = (scratch.path() / "absent.yaml").string();
    EXPECT_EQ(importCommand(toArgs({missing})), 1);
}

TEST(ImportCommand, imports_spec_and_writes_project) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    ScratchDir out;

    const int rc = importCommand(toArgs({spec.string(), "--out", out.path().string()}));

    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(fs::exists(out.path() / "reqloom.yaml"));
}

TEST(ImportCommand, refuses_to_overwrite_without_force) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    ScratchDir out;
    const auto args = toArgs({spec.string(), "--out", out.path().string()});

    ASSERT_EQ(importCommand(args), 0);
    // Second import into the same dir must fail rather than clobber.
    EXPECT_EQ(importCommand(args), 1);
}

TEST(ImportCommand, overwrites_with_force) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    ScratchDir out;

    ASSERT_EQ(importCommand(toArgs({spec.string(), "--out", out.path().string()})), 0);
    EXPECT_EQ(importCommand(toArgs({spec.string(), "--out", out.path().string(), "--force"})), 0);
}

TEST(ImportCommand, rejects_spec_outside_project_root) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    ScratchDir root;  // a different directory the spec does not live under
    ScratchDir out;

    const int rc = importCommand(toArgs(
        {spec.string(), "--project-root", root.path().string(), "--out", out.path().string()}));

    EXPECT_EQ(rc, 1);
}

}  // namespace reqloom::cli::tests
