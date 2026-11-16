// Public form-body preview tests — exercise reqloom/engine/FormBody.h, the
// stateless entry point an editor uses to preview how `body_form` encodes
// (urlencoded vs multipart) and to validate file references before a run.

#include <reqloom/engine/FormBody.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

/// A self-cleaning temp file holding `bytes`.
class TempFile {
public:
    explicit TempFile(const std::string& bytes) {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("reqloom-formbody-" + std::to_string(static_cast<long long>(stamp)) + "-" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        std::ofstream out{path_, std::ios::binary};
        out << bytes;
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }
    [[nodiscard]] std::string filename() const { return path_.filename().string(); }

private:
    fs::path path_;
};

}  // namespace

TEST(PreviewFormBody, plain_fields_are_url_encoded) {
    const auto out = ce::previewFormBody({{"a", "1"}, {"b", "two"}});
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->kind, ce::FormBodyKind::UrlEncoded);
    EXPECT_EQ(out->contentType, "application/x-www-form-urlencoded");
    EXPECT_TRUE(out->parts.empty());
    EXPECT_FALSE(out->urlEncoded.empty());
}

TEST(PreviewFormBody, multipart_header_routes_to_multipart) {
    const auto out =
        ce::previewFormBody({{"name", "ada"}}, {{"Content-Type", "multipart/form-data"}});
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->kind, ce::FormBodyKind::Multipart);
    ASSERT_EQ(out->parts.size(), 1u);
    EXPECT_EQ(out->parts.front().name, "name");
    EXPECT_FALSE(out->parts.front().isFile);
}

TEST(PreviewFormBody, file_reference_reports_part_metadata_not_bytes) {
    TempFile file{"hello upload"};  // 12 bytes
    const auto out = ce::previewFormBody({{"avatar", "@" + file.path()}, {"caption", "hi"}});
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->kind, ce::FormBodyKind::Multipart);

    const ce::FormPartPreview* avatar = nullptr;
    for (const auto& part : out->parts) {
        if (part.name == "avatar") {
            avatar = &part;
        }
    }
    ASSERT_NE(avatar, nullptr);
    EXPECT_TRUE(avatar->isFile);
    EXPECT_EQ(avatar->filename, file.filename());
    EXPECT_EQ(avatar->sizeBytes, 12u);
    EXPECT_FALSE(avatar->resolvedPath.empty());
}

TEST(PreviewFormBody, missing_file_is_upload_unreadable) {
    const auto out = ce::previewFormBody({{"doc", "@/no/such/file/anywhere.bin"}});
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ce::ErrorCode::UploadFileUnreadable);
}

TEST(PreviewFormBody, double_at_template_is_text_with_one_at_removed) {
    const auto out = ce::previewFormBody({{"handle", "@@mirza"}});
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->kind, ce::FormBodyKind::UrlEncoded);
    EXPECT_EQ(out->urlEncoded, "handle=%40mirza");
}

TEST(PreviewFormBody, relative_file_path_resolves_against_project_root) {
    TempFile file{"rooted"};
    const fs::path filePath{file.path()};
    const auto out =
        ce::previewFormBody({{"doc", "@" + file.filename()}}, {}, filePath.parent_path());
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_EQ(out->kind, ce::FormBodyKind::Multipart);
    ASSERT_EQ(out->parts.size(), 1u);
    EXPECT_TRUE(out->parts.front().isFile);
    EXPECT_TRUE(fs::equivalent(out->parts.front().resolvedPath, filePath));
    EXPECT_EQ(out->parts.front().sizeBytes, 6u);
}

TEST(FormTemplateText, only_a_single_leading_at_is_a_file) {
    EXPECT_TRUE(ce::isFormFileTemplate("@photo.png"));
    EXPECT_TRUE(ce::isFormFileTemplate("@"));
    EXPECT_FALSE(ce::isFormFileTemplate("@@mirza"));
    EXPECT_FALSE(ce::isFormFileTemplate("plain"));
    EXPECT_FALSE(ce::isFormFileTemplate(""));
}

TEST(FormTemplateText, text_with_leading_at_round_trips_through_the_escape) {
    EXPECT_EQ(ce::formTextTemplate("@mirza"), "@@mirza");
    EXPECT_EQ(ce::formTextTemplate("@@x"), "@@@x");
    EXPECT_EQ(ce::formTextTemplate("plain"), "plain");
    EXPECT_EQ(ce::formTemplateText("@@mirza"), "@mirza");
    EXPECT_EQ(ce::formTemplateText("plain"), "plain");
    EXPECT_EQ(ce::formTemplateText(ce::formTextTemplate("@@x")), "@@x");
    EXPECT_FALSE(ce::isFormFileTemplate(ce::formTextTemplate("@mirza")));
}
