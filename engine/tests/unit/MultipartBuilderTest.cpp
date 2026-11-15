// MultipartBuilderTest — exercises the routing rules and file-path
// validation that translate `body_form` into a request body.
//
// We don't talk to libcurl here; the contract under test is purely the
// shape of the FormBody variant and the error codes returned for bad
// file references.

#include "application/MultipartBuilder.h"

#include <reqloom/engine/ErrorCodes.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class TempFile {
public:
    explicit TempFile(std::string contents = "hello") {
        path_ = reqloom::tests::uniqueTempPath("reqloom-multipart", ".bin");
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
};

/// A self-cleaning directory, created empty.
class TempDir {
public:
    TempDir() : path_(reqloom::tests::uniqueTempPath("reqloom-mp-root")) {
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

/// A self-cleaning, uniquely named file in the current working directory.
class CwdFile {
public:
    explicit CwdFile(const std::string& contents)
        : path_(fs::current_path() /
                reqloom::tests::uniqueTempPath("reqloom-mp-cwd", ".txt").filename()) {
        std::ofstream out(path_, std::ios::binary);
        out << contents;
    }
    ~CwdFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    CwdFile(const CwdFile&) = delete;
    CwdFile& operator=(const CwdFile&) = delete;

    [[nodiscard]] std::string name() const { return path_.filename().string(); }

private:
    fs::path path_;
};

}  // namespace

TEST(MultipartBuilder, url_encoded_when_no_multipart_signals) {
    std::map<std::string, std::string> fields{{"name", "alice"}, {"qty", "5"}};
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/false, {});
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
    EXPECT_TRUE(ce::wantsMultipart(headers, ce::literalFormFields(fields)));
}

TEST(MultipartBuilder, text_only_fields_without_multipart_header_are_url_encoded) {
    const std::map<std::string, std::string> fields{{"note", "hello"}, {"sku", "abc"}};
    const auto classified = ce::literalFormFields(fields);

    const bool route = ce::wantsMultipart({}, classified);
    auto body = ce::buildFormBody(classified, route, {});

    EXPECT_FALSE(route);
    ASSERT_TRUE(body.has_value());
    EXPECT_NE(std::get_if<ce::UrlEncodedBody>(&*body), nullptr);
}

TEST(MultipartBuilder, multipart_when_value_starts_with_at) {
    std::map<std::string, std::string> headers{};
    std::map<std::string, std::string> fields{{"file", "@/tmp/x.txt"}};
    EXPECT_TRUE(ce::wantsMultipart(headers, ce::literalFormFields(fields)));
}

TEST(MultipartBuilder, builds_text_part_for_plain_value) {
    std::map<std::string, std::string> fields{{"name", "alice"}};
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
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

    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
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
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
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
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("does not exist"), std::string::npos);
}

TEST(MultipartBuilder, returns_upload_unreadable_when_path_is_directory) {
    std::map<std::string, std::string> fields{{"file", "@" + fs::temp_directory_path().string()}};
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("not a regular file"), std::string::npos);
}

TEST(MultipartBuilder, returns_upload_unreadable_when_at_prefix_has_empty_path) {
    std::map<std::string, std::string> fields{{"file", "@"}};
    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
}

TEST(MultipartBuilder, file_part_carries_preloaded_bytes) {
    // Regression: file uploads used to be deferred to libcurl via
    // curl_mime_filedata, which re-opened the path during send and
    // exposed a TOCTOU window between MultipartBuilder's stat and
    // libcurl's open. The fix loads bytes into MultipartPart::value at
    // build time so the wire layer never re-touches the path. This
    // test pins that behaviour: a file part now has both a filePath
    // (informational) AND its bytes already in `value`.
    TempFile tmp{"toctou-fixture-bytes-12345"};
    std::map<std::string, std::string> fields{{"document", "@" + tmp.path().string()}};

    auto body = ce::buildFormBody(ce::literalFormFields(fields), /*routeMultipart=*/true, {});
    ASSERT_TRUE(body.has_value()) << (body ? "" : body.error().detail);

    const auto* mb = std::get_if<ce::MultipartBody>(&*body);
    ASSERT_NE(mb, nullptr);
    ASSERT_EQ(mb->parts.size(), 1U);

    const auto& p = mb->parts[0];
    EXPECT_TRUE(p.isFile());
    EXPECT_EQ(p.value, "toctou-fixture-bytes-12345")
        << "file bytes must be pre-loaded into MultipartPart::value at build time";
}

TEST(MultipartBuilder, default_upload_limits_are_50_mib_per_part_and_256_mib_total) {
    constexpr ce::UploadLimits limits{};
    EXPECT_EQ(limits.perPartBytes, 50ULL * 1024 * 1024);
    EXPECT_EQ(limits.totalBytes, 256ULL * 1024 * 1024);
}

TEST(MultipartBuilder, fails_when_total_file_bytes_exceed_request_cap) {
    // Each part is under the per-part cap, but together they exceed the
    // total cap: the request must fail instead of holding every file in
    // memory. Limits are lowered so the test only writes a few bytes.
    TempFile a{std::string(6, 'a')};
    TempFile b{std::string(6, 'b')};
    std::map<std::string, std::string> fields{{"a", "@" + a.path().string()},
                                              {"b", "@" + b.path().string()}};

    auto body = ce::buildFormBody(ce::literalFormFields(fields),
                                  /*routeMultipart=*/true,
                                  {},
                                  ce::UploadLimits{.perPartBytes = 8, .totalBytes = 10});
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("total"), std::string::npos) << body.error().detail;
    EXPECT_NE(body.error().detail.find("12 bytes"), std::string::npos) << body.error().detail;
    EXPECT_NE(body.error().detail.find("10 bytes"), std::string::npos) << body.error().detail;
}

TEST(MultipartBuilder, accepts_files_whose_total_is_exactly_the_request_cap) {
    TempFile a{std::string(5, 'a')};
    TempFile b{std::string(5, 'b')};
    std::map<std::string, std::string> fields{{"a", "@" + a.path().string()},
                                              {"b", "@" + b.path().string()},
                                              {"note", "text parts do not count"}};

    auto body = ce::buildFormBody(ce::literalFormFields(fields),
                                  /*routeMultipart=*/true,
                                  {},
                                  ce::UploadLimits{.perPartBytes = 8, .totalBytes = 10});
    ASSERT_TRUE(body.has_value()) << body.error().detail;
}

TEST(MultipartBuilder, per_part_cap_still_applies_under_custom_limits) {
    TempFile a{std::string(9, 'a')};
    std::map<std::string, std::string> fields{{"a", "@" + a.path().string()}};

    auto body = ce::buildFormBody(ce::literalFormFields(fields),
                                  /*routeMultipart=*/true,
                                  {},
                                  ce::UploadLimits{.perPartBytes = 8, .totalBytes = 100});
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
    EXPECT_NE(body.error().detail.find("upload cap"), std::string::npos) << body.error().detail;
}

// --- File or text is decided from the template, not the resolved value ---

TEST(MultipartBuilder, single_at_template_is_a_file_field) {
    const auto t = ce::classifyFormTemplate("@{{env.dir}}/a.png");
    EXPECT_EQ(t.kind, ce::FormFieldKind::File);
    EXPECT_EQ(t.rest, "{{env.dir}}/a.png");
}

TEST(MultipartBuilder, double_at_template_is_text_with_one_at_removed) {
    const auto t = ce::classifyFormTemplate("@@mirza");
    EXPECT_EQ(t.kind, ce::FormFieldKind::Text);
    EXPECT_EQ(t.rest, "@mirza");
}

TEST(MultipartBuilder, variable_template_is_text_whatever_it_resolves_to) {
    const auto t = ce::classifyFormTemplate("{{x}}");
    EXPECT_EQ(t.kind, ce::FormFieldKind::Text);
    EXPECT_EQ(t.rest, "{{x}}");
}

TEST(MultipartBuilder, text_field_with_at_value_is_not_read_as_a_file) {
    TempFile secret{"top-secret-file-contents"};
    const ce::FormFields fields{
        {"note", ce::FormField{ce::FormFieldKind::Text, "@" + secret.path().string()}}};

    EXPECT_FALSE(ce::wantsMultipart({}, fields));
    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, {});
    ASSERT_TRUE(body.has_value()) << body.error().detail;
    const auto* mb = std::get_if<ce::MultipartBody>(&*body);
    ASSERT_NE(mb, nullptr);
    ASSERT_EQ(mb->parts.size(), 1U);
    EXPECT_FALSE(mb->parts[0].isFile());
    EXPECT_EQ(mb->parts[0].value, "@" + secret.path().string());
}

TEST(MultipartBuilder, file_field_routes_to_multipart) {
    const ce::FormFields fields{{"doc", ce::FormField{ce::FormFieldKind::File, "a.png"}}};
    EXPECT_TRUE(ce::wantsMultipart({}, fields));
}

// --- Where a file path is looked up ---

TEST(MultipartBuilder, relative_path_resolves_against_project_root) {
    const TempDir root;
    fs::create_directories(root.path() / "img");
    {
        std::ofstream out(root.path() / "img" / "photo.txt", std::ios::binary);
        out << "project-relative-bytes";
    }
    const ce::FormFields fields{{"doc", ce::FormField{ce::FormFieldKind::File, "img/photo.txt"}}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, root.path());
    ASSERT_TRUE(body.has_value()) << body.error().detail;
    const auto& part = std::get<ce::MultipartBody>(*body).parts.at(0);
    EXPECT_EQ(part.value, "project-relative-bytes");
    ASSERT_TRUE(part.filePath.has_value());
    EXPECT_TRUE(fs::equivalent(*part.filePath, root.path() / "img" / "photo.txt"));
}

TEST(MultipartBuilder, relative_path_is_not_found_in_cwd_when_project_root_is_set) {
    // A file that exists only in the current working directory must not be
    // picked up once the project root is known.
    const CwdFile inCwd{"cwd-bytes"};
    const TempDir root;
    const ce::FormFields fields{{"doc", ce::FormField{ce::FormFieldKind::File, inCwd.name()}}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, root.path());
    ASSERT_FALSE(body.has_value());
    EXPECT_EQ(body.error().code, ce::ErrorCode::UploadFileUnreadable);
}

TEST(MultipartBuilder, relative_path_falls_back_to_cwd_when_project_root_is_empty) {
    const CwdFile inCwd{"cwd-bytes"};
    const ce::FormFields fields{{"doc", ce::FormField{ce::FormFieldKind::File, inCwd.name()}}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, {});
    ASSERT_TRUE(body.has_value()) << body.error().detail;
    EXPECT_EQ(std::get<ce::MultipartBody>(*body).parts.at(0).value, "cwd-bytes");
}

TEST(MultipartBuilder, absolute_path_is_used_as_written_despite_project_root) {
    TempFile tmp{"absolute-bytes"};
    const TempDir root;
    const ce::FormFields fields{
        {"doc", ce::FormField{ce::FormFieldKind::File, tmp.path().string()}}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, root.path());
    ASSERT_TRUE(body.has_value()) << body.error().detail;
    const auto& part = std::get<ce::MultipartBody>(*body).parts.at(0);
    EXPECT_EQ(part.value, "absolute-bytes");
    ASSERT_TRUE(part.filePath.has_value());
    EXPECT_TRUE(fs::equivalent(*part.filePath, tmp.path()));
}

TEST(MultipartBuilder, tilde_is_not_expanded) {
    // `~/x` is a relative path whose first component is a directory named
    // `~`; it resolves under the project root like any other relative path.
    const TempDir root;
    fs::create_directories(root.path() / "~");
    {
        std::ofstream out(root.path() / "~" / "x.txt", std::ios::binary);
        out << "tilde-dir-bytes";
    }
    const ce::FormFields fields{{"doc", ce::FormField{ce::FormFieldKind::File, "~/x.txt"}}};

    auto body = ce::buildFormBody(fields, /*routeMultipart=*/true, root.path());
    ASSERT_TRUE(body.has_value()) << body.error().detail;
    EXPECT_EQ(std::get<ce::MultipartBody>(*body).parts.at(0).value, "tilde-dir-bytes");
}

TEST(MultipartBuilder, error_code_string_is_stable) {
    EXPECT_EQ(ce::toCodeString(ce::ErrorCode::UploadFileUnreadable), "E_UPLOAD_FILE_UNREADABLE");
}
