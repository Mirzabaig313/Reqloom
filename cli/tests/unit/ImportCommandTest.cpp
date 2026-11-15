// `reqloom import` command tests — exercise the argument contract and the
// import → writeProject glue end-to-end against a temp spec and output dir.
// The engine's importFromOpenApi/writeProject are covered separately; these
// assert the CLI's exit codes and on-disk effects.

#include "commands/ImportCommand.h"

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
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

/// A Postman collection with one bearer token pasted on two requests.
constexpr const char* kTokenCollection = R"JSON(
{
  "info": { "name": "Shop", "schema": "https://schema.getpostman.com/json/collection/v2.1.0/collection.json" },
  "item": [
    { "name": "One", "request": { "method": "GET", "url": "https://api.example.com/one",
      "header": [ { "key": "Authorization", "value": "Bearer abc123def456" } ] } },
    { "name": "Two", "request": { "method": "GET", "url": "https://api.example.com/two",
      "header": [ { "key": "Authorization", "value": "Bearer abc123def456" } ] } }
  ]
}
)JSON";

class MemoryStore final : public reqloom::engine::SecretStore {
public:
    std::map<std::string, std::string> values;
    bool failWrites{false};

    std::expected<std::optional<std::string>, reqloom::engine::ReqloomError> read(
        const std::string& key) override {
        if (const auto it = values.find(key); it != values.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, reqloom::engine::ReqloomError> write(const std::string& key,
                                                             const std::string& value) override {
        if (failWrites) {
            return std::unexpected(
                reqloom::engine::ReqloomError{reqloom::engine::ErrorCode::SecretAccessFailed,
                                              reqloom::engine::ErrorClass::Auth,
                                              "no keychain backend"});
        }
        values[key] = value;
        return {};
    }
    std::expected<void, reqloom::engine::ReqloomError> remove(const std::string& key) override {
        values.erase(key);
        return {};
    }
};

class MemoryTrust final : public reqloom::engine::ProjectTrustStore {
public:
    std::map<std::string, fs::path> folders;

    std::expected<std::optional<fs::path>, reqloom::engine::ReqloomError> folderFor(
        const std::string& id) override {
        if (const auto it = folders.find(id); it != folders.end()) {
            return std::optional<fs::path>{it->second};
        }
        return std::optional<fs::path>{};
    }
    std::expected<fs::path, reqloom::engine::ReqloomError> recordIfAbsent(
        const std::string& id, const fs::path& folder) override {
        return folders.try_emplace(id, folder).first->second;
    }
    std::expected<void, reqloom::engine::ReqloomError> rebind(const std::string& id,
                                                              const fs::path& folder) override {
        folders[id] = folder;
        return {};
    }
};

[[nodiscard]] std::string readFile(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Captured {
    int rc{0};
    std::string out;
    std::string err;
};

[[nodiscard]] Captured runCaptured(const QStringList& args,
                                   reqloom::engine::SecretStore* store,
                                   reqloom::engine::ProjectTrustStore* trust) {
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    Captured c;
    c.rc = reqloom::cli::importCommand(args, store, trust);
    std::fflush(stdout);
    std::fflush(stderr);
    c.out = testing::internal::GetCapturedStdout();
    c.err = testing::internal::GetCapturedStderr();
    return c;
}

}  // namespace

namespace reqloom::cli::tests {

TEST(ImportCommand, moves_a_shared_token_into_secrets_and_prints_names_only) {
    ScratchDir scratch;
    const auto spec = scratch.write("shop.json", kTokenCollection);
    ScratchDir out;
    MemoryStore store;
    MemoryTrust trust;

    const auto run =
        runCaptured(toArgs({spec.string(), "--out", out.path().string()}), &store, &trust);
    ASSERT_EQ(run.rc, 0) << run.err;
    const std::string all = run.out + run.err;
    EXPECT_NE(all.find("Moved 1 credential into the project's secrets: SHOP_AUTHORIZATION"),
              std::string::npos)
        << all;
    EXPECT_EQ(all.find("abc123def456"), std::string::npos) << all;
    ASSERT_EQ(store.values.size(), 1u);
    EXPECT_EQ(store.values.begin()->second, "abc123def456");
    EXPECT_EQ(readFile(out.path() / "resources" / "shop.yaml").find("abc123def456"),
              std::string::npos);
}

TEST(ImportCommand, no_secrets_flag_keeps_credentials_in_the_yaml_with_a_warning) {
    ScratchDir scratch;
    const auto spec = scratch.write("shop.json", kTokenCollection);
    ScratchDir out;
    MemoryStore store;
    MemoryTrust trust;

    const auto run = runCaptured(
        toArgs({spec.string(), "--out", out.path().string(), "--no-secrets"}), &store, &trust);
    ASSERT_EQ(run.rc, 0) << run.err;
    EXPECT_TRUE(store.values.empty());
    const std::string all = run.out + run.err;
    EXPECT_NE(all.find("Left 1 credential in the YAML in plain text"), std::string::npos) << all;
    EXPECT_NE(all.find("SHOP_AUTHORIZATION"), std::string::npos) << all;
    EXPECT_EQ(all.find("abc123def456"), std::string::npos) << all;
    EXPECT_NE(readFile(out.path() / "resources" / "shop.yaml").find("Bearer abc123def456"),
              std::string::npos);
}

TEST(ImportCommand, a_store_without_backend_keeps_credentials_and_says_why) {
    ScratchDir scratch;
    const auto spec = scratch.write("shop.json", kTokenCollection);
    ScratchDir out;
    MemoryStore store;
    store.failWrites = true;
    MemoryTrust trust;

    const auto run =
        runCaptured(toArgs({spec.string(), "--out", out.path().string()}), &store, &trust);
    ASSERT_EQ(run.rc, 0) << run.err;
    const std::string all = run.out + run.err;
    EXPECT_NE(all.find("no keychain backend"), std::string::npos) << all;
    EXPECT_EQ(all.find("abc123def456"), std::string::npos) << all;
    EXPECT_NE(readFile(out.path() / "resources" / "shop.yaml").find("abc123def456"),
              std::string::npos);
}

TEST(ImportCommand, missing_spec_argument_returns_usage_error) {
    EXPECT_EQ(importCommand(QStringList{}), 2);
}

TEST(ImportCommand, help_flag_returns_success) {
    EXPECT_EQ(importCommand(toArgs({"--help"})), 0);
}

TEST(ImportCommand, help_lists_every_detected_format) {
    testing::internal::CaptureStdout();
    std::ignore = importCommand(toArgs({"--help"}));
    std::fflush(stdout);
    const std::string help = testing::internal::GetCapturedStdout();

    for (const char* format : {"OpenAPI 3.x",
                               "Swagger 2.0",
                               "Postman",
                               "Insomnia",
                               "Thunder Client",
                               "Hoppscotch",
                               "HTTPie",
                               "Apidog",
                               ".http",
                               "Bruno"}) {
        EXPECT_NE(help.find(format), std::string::npos) << format << '\n' << help;
    }
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

// D-19: --force overwrites the files the import writes; it must never delete
// unrelated hand-written actors/resources/environments already in <dir>.
TEST(ImportCommand, force_keeps_unrelated_files_in_out_dir) {
    ScratchDir scratch;
    const auto spec = scratch.write("api.yaml", kMinimalSpec);
    ScratchDir out;
    ASSERT_EQ(importCommand(toArgs({spec.string(), "--out", out.path().string()})), 0);

    fs::create_directories(out.path() / "resources");
    fs::create_directories(out.path() / "actors");
    const auto keepRes = out.path() / "resources" / "keep.yaml";
    const auto keepActor = out.path() / "actors" / "admin.yaml";
    {
        std::ofstream res{keepRes};
        res << "resource: keep\n";
        std::ofstream actor{keepActor};
        actor << "actor: admin\n";
    }

    EXPECT_EQ(importCommand(toArgs({spec.string(), "--out", out.path().string(), "--force"})), 0);
    EXPECT_TRUE(fs::exists(keepRes));
    EXPECT_TRUE(fs::exists(keepActor));
    EXPECT_TRUE(fs::exists(out.path() / "reqloom.yaml"));
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
