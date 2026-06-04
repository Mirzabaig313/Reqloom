// Tests KeyValueEditor's read-back: rows → ordered std::map, blank-key drop,
// and the file-mode @path value. Drives the widget's public API directly.
#include "widgets/KeyValueEditor.h"

#include <gtest/gtest.h>

#include <QtWidgets/QLineEdit>

#include <map>
#include <string>

namespace chainapi::desktop::widgets::tests {

TEST(KeyValueEditor, round_trips_pairs_to_map) {
    KeyValueEditor editor;
    editor.setPairs({{QStringLiteral("Authorization"), QStringLiteral("Bearer x")},
                     {QStringLiteral("X-Trace"), QStringLiteral("abc")}});

    const auto map = editor.toStdMap();
    ASSERT_EQ(map.size(), 2u);
    EXPECT_EQ(map.at("Authorization"), "Bearer x");
    EXPECT_EQ(map.at("X-Trace"), "abc");
}

TEST(KeyValueEditor, blank_key_rows_are_dropped) {
    KeyValueEditor editor;
    editor.setPairs({{QStringLiteral(""), QStringLiteral("orphan")},
                     {QStringLiteral("keep"), QStringLiteral("v")}});

    const auto map = editor.toStdMap();
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains("keep"));
}

TEST(KeyValueEditor, empty_editor_reports_no_content) {
    KeyValueEditor editor;
    EXPECT_TRUE(editor.isEmptyOfContent());
    editor.setPairs({{QStringLiteral("k"), QStringLiteral("v")}});
    EXPECT_FALSE(editor.isEmptyOfContent());
}

TEST(KeyValueEditor, file_mode_preserves_at_path_value) {
    KeyValueEditor editor;
    editor.setMode(KeyValueEditor::Mode::FileCapable);
    // A value already in @path form (as the file picker would write) must
    // survive the read-back so the engine sees the upload reference.
    editor.setPairs({{QStringLiteral("avatar"), QStringLiteral("@/tmp/pic.png")}});

    const auto map = editor.toStdMap();
    ASSERT_EQ(map.size(), 1u);
    EXPECT_EQ(map.at("avatar"), "@/tmp/pic.png");
}

TEST(KeyValueEditor, keeps_one_trailing_blank_row_for_auto_grow) {
    // The Apidog-style ghost row means a fresh editor already has a place to
    // type, and setting pairs leaves exactly one trailing blank row — so the
    // count of QLineEdit key fields is pairs + 1, while toStdMap drops blanks.
    KeyValueEditor editor;
    const auto keyFieldCount = [&editor]() {
        int n = 0;
        for (auto* f : editor.findChildren<QLineEdit*>(QStringLiteral("kvKey"))) {
            (void)f;
            ++n;
        }
        return n;
    };

    EXPECT_EQ(keyFieldCount(), 1);  // ghost row on construction
    EXPECT_TRUE(editor.isEmptyOfContent());

    editor.setPairs(
        {{QStringLiteral("a"), QStringLiteral("1")}, {QStringLiteral("b"), QStringLiteral("2")}});
    EXPECT_EQ(keyFieldCount(), 3);  // two populated + one trailing ghost
    EXPECT_EQ(editor.toStdMap().size(), 2u);
}

}  // namespace chainapi::desktop::widgets::tests
