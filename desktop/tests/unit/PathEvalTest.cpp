// Checks the pure extraction-path classifier behind the chain editor's inline
// validity indicator. The Qt wrapper (AppController) only chooses the body.
#include "views/PathEval.h"

#include <gtest/gtest.h>

namespace reqloom::desktop::views::tests {

TEST(PathEval, matching_path_returns_first_value) {
    const auto r = classifyExtractionPath(R"({"data":{"id":"9f7e"}})", "$.data.id");
    EXPECT_EQ(r.state, PathState::Match);
    EXPECT_EQ(r.value, "9f7e");
}

TEST(PathEval, valid_body_but_missing_path_is_nomatch) {
    const auto r = classifyExtractionPath(R"({"data":{}})", "$.data.id");
    EXPECT_EQ(r.state, PathState::NoMatch);
}

TEST(PathEval, header_path_is_neutral_not_flagged) {
    const auto r = classifyExtractionPath(R"({"data":{"id":1}})", "$.headers.X-Token");
    EXPECT_EQ(r.state, PathState::Neutral);
}

TEST(PathEval, empty_body_or_path_is_neutral) {
    EXPECT_EQ(classifyExtractionPath("", "$.data.id").state, PathState::Neutral);
    EXPECT_EQ(classifyExtractionPath(R"({"a":1})", "").state, PathState::Neutral);
}

TEST(PathEval, collect_paths_returns_leaf_paths_filtered) {
    const QString body = R"({"data":{"id":"x","items":[{"id":1}]},"ok":true})";
    // No filter → every leaf path, in bare (no leading "$.") form.
    const QStringList all = collectJsonPaths(body, QString());
    EXPECT_TRUE(all.contains("data.id"));
    EXPECT_TRUE(all.contains("data.items[0].id"));
    EXPECT_TRUE(all.contains("ok"));
    // Filter narrows (case-insensitive substring).
    const QStringList items = collectJsonPaths(body, "items");
    EXPECT_EQ(items, QStringList{"data.items[0].id"});
    // Invalid JSON → empty.
    EXPECT_TRUE(collectJsonPaths("not json", QString()).isEmpty());
}

}  // namespace reqloom::desktop::views::tests
