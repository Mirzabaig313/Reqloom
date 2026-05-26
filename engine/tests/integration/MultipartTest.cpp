// MultipartTest — confirms that:
//   1. body_form with @path values triggers multipart/form-data
//   2. The mock SUT actually receives the file bytes as a multipart part
//   3. body_form without file refs and without a multipart header still
//      uses application/x-www-form-urlencoded (regression cover for the
//      previous behavior)
//
// Files under test reach libcurl's mime API, so this test exercises the
// real network path end-to-end.
#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] fs::path projectDir() {
    return fixturesDir() / "multipart-project";
}

class TempUploadFile {
public:
    explicit TempUploadFile(std::string contents) : contents_(std::move(contents)) {
        path_ = fs::temp_directory_path() / ("chainapi-mp-" + std::to_string(::getpid()) + "-" +
                                             std::to_string(counter_++) + ".bin");
        std::ofstream out(path_, std::ios::binary);
        out << contents_;
    }
    ~TempUploadFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempUploadFile(const TempUploadFile&) = delete;
    TempUploadFile& operator=(const TempUploadFile&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& contents() const noexcept { return contents_; }

private:
    fs::path path_;
    std::string contents_;
    static inline int counter_{0};
};

/// Fetch /__mock/last-request?path=<p> and parse it. Tiny ad-hoc curl
/// helper so the test can read the mock SUT's capture log without
/// pulling in the engine's internal HttpClient (which would require
/// piercing the engine's private headers).
[[nodiscard]] nlohmann::json fetchLastRequest(const std::string& baseUrl, const std::string& path) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), &curl_easy_cleanup};
    EXPECT_NE(curl.get(), nullptr);

    const auto url = baseUrl + "/__mock/last-request?path=" + path;
    std::string body;
    auto writer = +[](char* ptr, std::size_t size, std::size_t nmemb, void* ud) -> std::size_t {
        auto* out = static_cast<std::string*>(ud);
        out->append(ptr, size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writer);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    auto rc = curl_easy_perform(curl.get());
    EXPECT_EQ(rc, CURLE_OK) << curl_easy_strerror(rc);

    return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl, const std::string& filePath) {
    auto project = ce::parseProject(projectDir() / "chainapi.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    auto& env = project->environments["local"];
    env["baseUrl"] = mockBaseUrl;
    env["uploadFile"] = filePath;
    return std::move(*project);
}

}  // namespace

class MultipartFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "multipart-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(MultipartFixture, file_upload_sends_multipart_with_file_bytes) {
    TempUploadFile tmp{"chainapi-multipart-fixture-bytes"};

    auto project = loadProject(harness_->baseUrl(), tmp.path().string());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"upload.with_file"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded()) << "upload chain did not succeed";

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/uploads");
    ASSERT_TRUE(cap.is_object());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["method"].get<std::string>(), "POST");

    const std::string contentType = cap["content_type"].get<std::string>();
    EXPECT_NE(contentType.find("multipart/form-data"), std::string::npos)
        << "request was not multipart, content_type=" << contentType;

    const auto& parts = cap["parts"];
    ASSERT_TRUE(parts.is_array());
    ASSERT_EQ(parts.size(), 2U) << "expected documentType + file parts";

    bool sawDocType = false;
    bool sawFile = false;
    for (const auto& p : parts) {
        const auto name = p["name"].get<std::string>();
        if (name == "documentType") {
            EXPECT_EQ(p["content_text"].get<std::string>(), "passport");
            sawDocType = true;
        } else if (name == "file") {
            EXPECT_EQ(p["filename"].get<std::string>(), tmp.path().filename().string());
            EXPECT_EQ(p["content_text"].get<std::string>(), tmp.contents())
                << "file part bytes did not match the uploaded file";
            sawFile = true;
        }
    }
    EXPECT_TRUE(sawDocType);
    EXPECT_TRUE(sawFile);
}

TEST_F(MultipartFixture, plain_form_still_uses_url_encoded_body) {
    auto project = loadProject(harness_->baseUrl(), "/dev/null-not-used");
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"form.url_encoded"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded());

    auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/forms");
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["content_type"].get<std::string>(), "application/x-www-form-urlencoded");
    EXPECT_TRUE(cap["parts"].is_array());
    EXPECT_EQ(cap["parts"].size(), 0U) << "url-encoded body must not register multipart parts";
}

TEST_F(MultipartFixture, missing_upload_file_fails_with_upload_unreadable) {
    auto project = loadProject(harness_->baseUrl(), "/no/such/path/chainapi-test-missing-XXX.bin");
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"upload.with_file"}, ctx);
    ASSERT_TRUE(result.has_value()) << "engine itself returned an error";
    EXPECT_FALSE(result->succeeded());

    bool sawUploadError = false;
    for (const auto& step : result->steps) {
        if (step.op.value == "upload.with_file" && step.status == ce::StepResult::Status::Failed) {
            EXPECT_EQ(step.error, ce::ErrorCode::UploadFileUnreadable);
            sawUploadError = true;
        }
    }
    EXPECT_TRUE(sawUploadError);
}
