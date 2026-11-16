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

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

[[nodiscard]] fs::path projectDir() {
    return fixturesDir() / "multipart-project";
}

class TempUploadFile {
public:
    explicit TempUploadFile(std::string contents) : contents_(std::move(contents)) {
        path_ = ct::uniqueTempPath("reqloom-mp", ".bin");
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
    auto project = ce::parseProject(projectDir() / "reqloom.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    auto& env = project->environments["local"];
    env["baseUrl"] = mockBaseUrl;
    env["uploadFile"] = filePath;
    return std::move(*project);
}

/// A self-cleaning directory, created empty, standing in for a project folder.
class TempProjectDir {
public:
    TempProjectDir() : path_(ct::uniqueTempPath("reqloom-mp-project")) {
        fs::create_directories(path_);
    }
    ~TempProjectDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempProjectDir(const TempProjectDir&) = delete;
    TempProjectDir& operator=(const TempProjectDir&) = delete;

    /// Write `contents` to `name` inside the directory; returns its path.
    fs::path write(const std::string& name, const std::string& contents) const {
        const auto p = path_ / name;
        std::ofstream out(p, std::ios::binary);
        out << contents;
        return p;
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

/// Replace the `body_form` of `form.url_encoded` (an op with no
/// Content-Type header, so multipart is chosen only by the fields).
void setPlainFormFields(ce::Project& project, std::map<std::string, std::string> fields) {
    project.resources.at(ce::ResourceId{"form"}).operations.at("url_encoded").bodyForm =
        std::move(fields);
}

/// Run `form.url_encoded` and return the captured request.
[[nodiscard]] nlohmann::json runPlainForm(const ce::Project& project, const std::string& baseUrl) {
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"form.url_encoded"}, ctx);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    if (result) {
        EXPECT_TRUE(result->succeeded());
        for (const auto& step : result->steps) {
            EXPECT_EQ(step.status, ce::StepResult::Status::Succeeded)
                << step.op.value << " failed: " << step.detail;
        }
    }
    return fetchLastRequest(baseUrl, "/api/v1/forms");
}

/// The single part named `name` in a captured multipart request.
[[nodiscard]] nlohmann::json partNamed(const nlohmann::json& cap, const std::string& name) {
    for (const auto& p : cap["parts"]) {
        if (p["name"].get<std::string>() == name) {
            return p;
        }
    }
    ADD_FAILURE() << "no part named " << name << " in " << cap.dump();
    return nlohmann::json::object();
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
    TempUploadFile tmp{"reqloom-multipart-fixture-bytes"};

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
    auto project = loadProject(harness_->baseUrl(), "/no/such/path/reqloom-test-missing-XXX.bin");
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

// --- Where a relative upload path is looked up ---

TEST_F(MultipartFixture, relative_upload_path_is_found_in_the_project_folder) {
    const TempProjectDir dir;
    dir.write("photo.txt", "bytes-from-the-project-folder");
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.rootDir = dir.path();
    setPlainFormFields(project, {{"file", "@photo.txt"}});

    const auto cap = runPlainForm(project, harness_->baseUrl());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_NE(cap["content_type"].get<std::string>().find("multipart/form-data"), std::string::npos)
        << "a file field written as `@...` must route to multipart";
    const auto part = partNamed(cap, "file");
    EXPECT_EQ(part.value("filename", ""), "photo.txt");
    EXPECT_EQ(part.value("content_text", ""), "bytes-from-the-project-folder");
}

TEST_F(MultipartFixture, relative_upload_path_falls_back_to_cwd_without_project_root) {
    const auto name = ct::uniqueTempPath("reqloom-mp-cwd", ".txt").filename();
    const auto inCwd = fs::current_path() / name;
    {
        std::ofstream out(inCwd, std::ios::binary);
        out << "bytes-from-the-cwd";
    }
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.rootDir.clear();
    setPlainFormFields(project, {{"file", "@" + name.string()}});

    const auto cap = runPlainForm(project, harness_->baseUrl());
    std::error_code ec;
    fs::remove(inCwd, ec);
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(partNamed(cap, "file").value("content_text", ""), "bytes-from-the-cwd");
}

TEST_F(MultipartFixture, upload_path_built_from_a_variable_after_the_at_uploads) {
    const TempProjectDir dir;
    dir.write("name.txt", "bytes-via-variable-path");
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.environments["local"]["dir"] = dir.path().string();
    setPlainFormFields(project, {{"file", "@{{env.dir}}/name.txt"}});

    const auto cap = runPlainForm(project, harness_->baseUrl());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(partNamed(cap, "file").value("content_text", ""), "bytes-via-variable-path");
}

// --- File or text is decided from the template, not the resolved value ---

TEST_F(MultipartFixture, variable_resolving_to_at_path_is_sent_as_text_not_a_file) {
    TempUploadFile secret{"secret-file-contents-must-not-leave"};
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.environments["local"]["sneaky"] = "@" + secret.path().string();
    setPlainFormFields(project, {{"note", "{{env.sneaky}}"}});

    const auto cap = runPlainForm(project, harness_->baseUrl());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["content_type"].get<std::string>(), "application/x-www-form-urlencoded")
        << "a text field must not switch the body to multipart";
    const auto raw = cap["raw_body"].get<std::string>();
    EXPECT_EQ(raw.find(secret.contents()), std::string::npos) << raw;
    EXPECT_TRUE(raw.starts_with("note=%40")) << raw;
}

TEST_F(MultipartFixture, variable_resolving_to_at_path_is_a_text_part_under_multipart_header) {
    TempUploadFile secret{"secret-file-contents-must-not-leave"};
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.environments["local"]["sneaky"] = "@" + secret.path().string();
    project.resources.at(ce::ResourceId{"upload"}).operations.at("with_file").bodyForm =
        std::map<std::string, std::string>{{"documentType", "{{env.sneaky}}"}};

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"upload.with_file"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->succeeded());

    const auto cap = fetchLastRequest(harness_->baseUrl(), "/api/v1/uploads");
    ASSERT_TRUE(cap["found"].get<bool>());
    const auto part = partNamed(cap, "documentType");
    EXPECT_EQ(part.value("filename", ""), "");
    EXPECT_EQ(part.value("content_text", ""), "@" + secret.path().string());
    EXPECT_EQ(cap.dump().find(secret.contents()), std::string::npos) << cap.dump();
}

TEST_F(MultipartFixture, double_at_sends_literal_text_starting_with_at) {
    auto project = loadProject(harness_->baseUrl(), "unused");
    setPlainFormFields(project, {{"handle", "@@handle"}});

    const auto cap = runPlainForm(project, harness_->baseUrl());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(cap["content_type"].get<std::string>(), "application/x-www-form-urlencoded");
    EXPECT_EQ(cap["raw_body"].get<std::string>(), "handle=%40handle");
}

TEST_F(MultipartFixture, preview_reports_the_path_the_run_uploads) {
    const TempProjectDir dir;
    const auto file = dir.write("photo.txt", "preview-and-run-bytes");
    auto project = loadProject(harness_->baseUrl(), "unused");
    project.rootDir = dir.path();
    const std::map<std::string, std::string> fields{{"file", "@photo.txt"}};
    setPlainFormFields(project, fields);

    const auto preview = ce::previewFormBody(fields, {}, project.rootDir);
    ASSERT_TRUE(preview.has_value()) << preview.error().detail;
    ASSERT_EQ(preview->parts.size(), 1U);
    EXPECT_TRUE(fs::equivalent(preview->parts.front().resolvedPath, file));

    const auto cap = runPlainForm(project, harness_->baseUrl());
    ASSERT_TRUE(cap["found"].get<bool>());
    EXPECT_EQ(partNamed(cap, "file").value("content_text", ""), "preview-and-run-bytes");
}
