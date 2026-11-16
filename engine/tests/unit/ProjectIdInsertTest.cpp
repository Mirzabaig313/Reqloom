// ProjectIdInsertTest — `insertProjectIdLine` adds exactly one top-level
// `id:` line to reqloom.yaml text and leaves every other byte alone, or
// refuses (no change) when the root cannot be edited safely as text.
#include <reqloom/engine/ProjectIdentity.h>

#include <gtest/gtest.h>

#include <string>

namespace ce = reqloom::engine;

namespace {

constexpr const char* kId = "0b9f6a52-3c1d-4e8f-9a7b-1c2d3e4f5a6b";

[[nodiscard]] std::string inserted(const std::string& text) {
    auto result = ce::insertProjectIdLine(text, kId);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    if (!result || !result->has_value()) {
        ADD_FAILURE() << "expected an edit";
        return text;
    }
    return **result;
}

void expectRefused(const std::string& text) {
    auto result = ce::insertProjectIdLine(text, kId);
    ASSERT_FALSE(result.has_value()) << "edited unsafe text:\n" << text;
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_FALSE(result.error().detail.empty());
}

}  // namespace

TEST(ProjectIdInsert, goes_right_after_name) {
    EXPECT_EQ(inserted("version: 1\nname: Shop\ndefault_environment: local\n"),
              std::string{"version: 1\nname: Shop\nid: "} + kId + "\ndefault_environment: local\n");
}

TEST(ProjectIdInsert, name_first_line) {
    EXPECT_EQ(inserted("name: Shop  # the shop\nversion: 1\n"),
              std::string{"name: Shop  # the shop\nid: "} + kId + "\nversion: 1\n");
}

TEST(ProjectIdInsert, skips_a_multi_line_name_value) {
    const std::string text = "name: |\n  Line one\n\n  Line two\n\n# about version\nversion: 1\n";
    EXPECT_EQ(inserted(text),
              std::string{"name: |\n  Line one\n\n  Line two\nid: "} + kId +
                  "\n\n# about version\nversion: 1\n");
}

TEST(ProjectIdInsert, without_name_goes_after_leading_comments_and_marker) {
    const std::string text = "# Shop API\n# owned by team x\n---\nversion: 1\nresources: {}\n";
    EXPECT_EQ(inserted(text),
              std::string{"# Shop API\n# owned by team x\n---\nid: "} + kId +
                  "\nversion: 1\nresources: {}\n");
}

TEST(ProjectIdInsert, keeps_comments_everywhere_else) {
    const std::string text = "# top\nversion: 1 # v\n# before name\nname: Shop\n# after\n";
    EXPECT_EQ(inserted(text),
              std::string{"# top\nversion: 1 # v\n# before name\nname: Shop\nid: "} + kId +
                  "\n# after\n");
}

TEST(ProjectIdInsert, keeps_crlf_line_endings) {
    EXPECT_EQ(inserted("version: 1\r\nname: Shop\r\nresources: {}\r\n"),
              std::string{"version: 1\r\nname: Shop\r\nid: "} + kId + "\r\nresources: {}\r\n");
}

TEST(ProjectIdInsert, no_trailing_newline_stays_without_one) {
    EXPECT_EQ(inserted("version: 1\nname: Shop"),
              std::string{"version: 1\nname: Shop\nid: "} + kId);
    EXPECT_EQ(inserted("version: 1\r\nname: Shop"),
              std::string{"version: 1\r\nname: Shop\r\nid: "} + kId);
}

TEST(ProjectIdInsert, keeps_a_utf8_bom) {
    EXPECT_EQ(inserted("\xEF\xBB\xBFversion: 1\n"),
              std::string{"\xEF\xBB\xBFid: "} + kId + "\nversion: 1\n");
}

TEST(ProjectIdInsert, existing_id_means_no_change) {
    for (const std::string text : {"name: Shop\nid: abc\n",
                                   "id: abc\nname: Shop\n",
                                   "name: Shop\n\"id\": abc\n",
                                   "name: Shop\nid : abc\n"}) {
        auto result = ce::insertProjectIdLine(text, kId);
        ASSERT_TRUE(result.has_value()) << text;
        EXPECT_FALSE(result->has_value()) << text;
    }
}

TEST(ProjectIdInsert, nested_id_does_not_count_as_root_id) {
    const std::string text = "name: Shop\nresources:\n  item:\n    id: nested\n";
    EXPECT_EQ(inserted(text),
              std::string{"name: Shop\nid: "} + kId + "\nresources:\n  item:\n    id: nested\n");
}

TEST(ProjectIdInsert, flow_style_root_is_refused) {
    expectRefused("{name: Shop, version: 1}\n");
    expectRefused("---\n{name: Shop}\n");
}

TEST(ProjectIdInsert, odd_roots_are_refused) {
    expectRefused("- name: Shop\n");                  // sequence root
    expectRefused("? name\n: Shop\n");                // complex key
    expectRefused("--- !project\nname: Shop\n");      // tagged document
    expectRefused("&root\nname: Shop\n");             // anchored root
    expectRefused("name: Shop\n---\nname: Other\n");  // two documents
    expectRefused("# only a comment\n");              // no top-level key
    expectRefused("");                                // empty
    expectRefused("  name: Shop\n  version: 1\n");    // indented root
    expectRefused("\tname: Shop\n");                  // tab-indented root
}

TEST(ProjectIdInsert, id_that_is_not_a_plain_scalar_is_refused) {
    auto result = ce::insertProjectIdLine("name: Shop\n", "a: b");
    ASSERT_FALSE(result.has_value());
    auto hash = ce::insertProjectIdLine("name: Shop\n", "#x");
    ASSERT_FALSE(hash.has_value());
}
