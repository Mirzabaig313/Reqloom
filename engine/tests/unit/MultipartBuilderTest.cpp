// MultipartBuilderTest — exercises the routing rules and file-path
// validation that translate `body_form` into a request body.
//
// We don't talk to libcurl here; the contract under test is purely the
// shape of the FormBody variant and the error codes returned for bad
// file references.

#include "application/MultipartBuilder.h"

#include <chainapi/engine/ErrorCodes.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class TempFile {
public:
    explicit TempFile(std::string contents = "hello") {
        path_ = fs::temp_directory_path() /
                fs::path("chainapi-multipart-" + std::to_string(::getpid()) + "-" +
                         std::to_string(counter_++) + ".bin");
        std::ofstream out(path_, std::ios::binary);
        out << contents;
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
    static inline int counter_{0};
};

}  // namespace

TEST(MultipartBuilder, url_encoded_when_no_multipart_signals) {
    std::map<std::string, std::string> fields{{"name", "alice"}, {"qty", "5"}};
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/false);
    ASSERT_TRUE(body.has_value());

    const auto* enc = std::get_if<ce::UrlEncodedBody>(&*body);
    ASSERT_NE(enc, nullptr);
    EXPECT_NE(enc->body.find("name=alice"), std::string::npos);
    EXPECT_NE(enc->body.find("qty=5"), std::string::npos);
    EXPECT_NE(enc->body.find("&"), std::string::npos);
}

TEST(MultipartBuilder, multipart_when_header_routes_request) {
    std::map<std::string, std::string> headers{{"Content-Type", "multipart/form-data"}};
    std::map<std::string, std::string> fields{{"qty", "5"}, {"sku", "abc"}};
    EXPECT_TRUE(ce::wantsMultipart(headers, fields));
}

TEST(MultipartBuilder, multipart_when_value_starts_with_at) {
    std::map<std::string, std::string> headers{};
    std::map<std::string, std::string> fields{{"file", "@/tmp/x.txt"}};
    EXPECT_TRUE(ce::wantsMultipart(headers, fields));
}

TEST(MultipartBuilder, builds_text_part_for_plain_value) {
    std::map<std::string, std::string> fields{{"name", "alice"}};
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_TRUE(body.has_value());

    const auto* mb = std::get_if<ce::MultipartBody>(&*body);
    ASSERT_NE(mb, nullptr);
    ASSERT_EQ(mb->parts.size(), 1U);
    EXPECT_EQ(mb->parts[0].name, "name");
    EXPECT_EQ(mb->parts[0].value, "alice");
    EXPECT_FALSE(mb->parts[0].isFile());
}

TEST(MultipartBuilder, builds_file_part_for_at_prefixed_value) {
    TempFile tmp{"upload-bytes"};
    std::map<std::string, std::string> fields{{"document", "@" + tmp.path().string()}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_TRUE(body.has_value()) << (body ? "" : body.error().detail);

    const auto* mb = std::get_if<ce::MultipartBody>(&*body);
    ASSERT_NE(mb, nullptr);
    ASSERT_EQ(mb->parts.size(), 1U);
    const auto& p = mb->parts[0];
    EXPECT_EQ(p.name, "document");
    EXPECT_TRUE(p.isFile());
    ASSERT_TRUE(p.filePath.has_value());
    EXPECT_TRUE(fs::equivalent(*p.filePath, tmp.path()))
        << "file part path should canonicalise to the same file";
    ASSERT_TRUE(p.filename.has_value());
    EXPECT_EQ(*p.filename, tmp.path().filename().string());
}

TEST(MultipartBuilder, mixes_text_and_file_parts) {
    TempFile tmp{"bytes"};
    std::map<std::string, std::string> fields{
        {"documentType", "passport"},
        {"file", "@" + tmp.path().string()},
    };
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_TRUE(body.has_value());
    const auto* mb = std::get_if<ce::MultipartBody>(&*body);
    ASSERT_NE(mb, nullptr);
    ASSERT_EQ(mb->parts.size(), 2U);

    bool sawText = false;
    bool sawFile = false;
    for (const auto& p : mb->parts) {
        if (p.name == "documentType") {
            EXPECT_EQ(p.value, "passport");
            EXPECT_FALSE(p.isFile());
            sawText = true;
        } else if (p.name == "file") {
            EXPECT_TRUE(p.isFile());
            sawFile = true;
        }
    }
    EXPECT_TRUE(sawText);
    EXPECT_TRUE(sawFile);
}

TEST(MultipartBuilder, returns_upload_unreadable_when_file_missing) {
    std::map<std::string, std::string> fields{{"file", "@/no/such/path/should-exist-XXX.bin"}};
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("does not exist"), std::string::npos);
}

TEST(MultipartBuilder, returns_upload_unreadable_when_path_is_directory) {
    std::map<std::string, std::string> fields{{"file", "@" + fs::temp_directory_path().string()}};
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("not a regular file"), std::string::npos);
}

TEST(MultipartBuilder, returns_upload_unreadable_when_at_prefix_has_empty_path) {
    std::map<std::string, std::string> fields{{"file", "@"}};
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true);
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
}

TEST(MultipartBuilder, error_code_string_is_stable) {
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::UploadFileUnreadable), "E_UPLOAD_FILE_UNREADABLE");
}
