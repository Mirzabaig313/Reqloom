// Request override path/query canonicalization coverage. Desktop Requirement §6.2.
#include "appcontroller/AppControllerInternal.h"
#include "application/RunController.h"

#include <gtest/gtest.h>

#include <QtCore/QString>

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::desktop::tests {

TEST(RequestOverride, moves_embedded_query_into_structured_params) {
    RequestOverride requestOverride;
    requestOverride.path = QStringLiteral("/api/testing?empty=&testingpara=6#ignored");
    requestOverride.queryParams = {{"keep", "yes"}, {"testingpara", "old"}};

    requestOverride.normalizePathQuery();

    EXPECT_EQ(requestOverride.path, QStringLiteral("/api/testing#ignored"));
    EXPECT_EQ(
        requestOverride.queryParams,
        (std::map<std::string, std::string>{{"empty", ""}, {"keep", "yes"}, {"testingpara", "6"}}));
}

TEST(RequestOverride, keeps_percent_escaped_query_in_the_raw_path) {
    const std::array paths{
        QStringLiteral("/search?delimiter=a%26b%3Dc&plain=ok&twice=%2520#results"),
        QStringLiteral("/search?lower=a%2fb"),
        QStringLiteral("/search?mixed=a%2Fb%3dc")};

    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        RequestOverride requestOverride;
        requestOverride.path = path;

        requestOverride.normalizePathQuery();

        EXPECT_EQ(requestOverride.path, path);
        EXPECT_TRUE(requestOverride.queryParams.empty());
    }
}

TEST(RequestOverride, keeps_percent_encoded_braces_in_the_raw_path) {
    RequestOverride requestOverride;
    requestOverride.path = QStringLiteral("/search?literal=%7B%7Bsecret.value%7D%7D");

    requestOverride.normalizePathQuery();

    EXPECT_EQ(requestOverride.path, QStringLiteral("/search?literal=%7B%7Bsecret.value%7D%7D"));
    EXPECT_TRUE(requestOverride.queryParams.empty());
}

TEST(RequestOverride, keeps_query_templates_in_the_raw_path) {
    const std::array paths{
        QStringLiteral("/search?encoded={{env.encoded}}&next=2"),
        QStringLiteral("/search?q={{$.url.encode(\"a\\\"}}?b&c=d#e\")}}&next=2#results")};

    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        RequestOverride requestOverride;
        requestOverride.path = path;

        requestOverride.normalizePathQuery();

        EXPECT_EQ(requestOverride.path, path);
        EXPECT_TRUE(requestOverride.queryParams.empty());
    }
}

TEST(RequestOverride, preserves_query_shapes_that_a_map_cannot_represent) {
    const std::array paths{QStringLiteral("/search?duplicate=1&duplicate=2"),
                           QStringLiteral("/search?z=1&a=2"),
                           QStringLiteral("/search?flag"),
                           QStringLiteral("/search?=empty"),
                           QStringLiteral("/search?plus=a+b"),
                           QStringLiteral("/search#fragment?admin=true")};

    for (const QString& path : paths) {
        SCOPED_TRACE(path.toStdString());
        RequestOverride requestOverride;
        requestOverride.path = path;
        requestOverride.queryParams = {{"keep", "yes"}};

        requestOverride.normalizePathQuery();

        EXPECT_EQ(requestOverride.path, path);
        EXPECT_EQ(requestOverride.queryParams,
                  (std::map<std::string, std::string>{{"keep", "yes"}}));
    }
}

TEST(RequestQuerySync, projects_visible_url_into_ordered_params) {
    const auto pairs =
        qml::queryPairsFromVisiblePath(QStringLiteral("/api/testing?param=3&paramtwo=#ignored"));

    EXPECT_EQ(
        pairs,
        (std::vector<std::pair<QString, QString>>{{QStringLiteral("param"), QStringLiteral("3")},
                                                  {QStringLiteral("paramtwo"), QString{}}}));
}

TEST(RequestQuerySync, preserves_duplicate_order_and_decodes_components) {
    const auto pairs = qml::queryPairsFromVisiblePath(QStringLiteral(
        "/search?item=first&item=second&delimiter=a%26b%3Dc&flag#ignored?not=query"));

    EXPECT_EQ(pairs,
              (std::vector<std::pair<QString, QString>>{
                  {QStringLiteral("item"), QStringLiteral("first")},
                  {QStringLiteral("item"), QStringLiteral("second")},
                  {QStringLiteral("delimiter"), QStringLiteral("a&b=c")},
                  {QStringLiteral("flag"), QString{}}}));
}

TEST(RequestQuerySync, params_replace_query_with_correct_delimiters) {
    const std::vector<std::pair<QString, QString>> pairs{
        {QStringLiteral("param"), QStringLiteral("3")}, {QStringLiteral("paramtwo"), QString{}}};

    EXPECT_EQ(qml::visiblePathWithQueryPairs(QStringLiteral("/api/testing#fragment"), pairs),
              QStringLiteral("/api/testing?param=3&paramtwo=#fragment"));
    EXPECT_EQ(qml::visiblePathWithQueryPairs(QStringLiteral("/api/testing?old=1"), {}),
              QStringLiteral("/api/testing"));
}

TEST(RequestQuerySync, params_encode_reserved_values_and_preserve_templates) {
    const std::vector<std::pair<QString, QString>> pairs{
        {QStringLiteral("delimiter"), QStringLiteral("a&b=c")},
        {QStringLiteral("plus"), QStringLiteral("a+b")},
        {QStringLiteral("space"), QStringLiteral("a b")},
        {QStringLiteral("template"), QStringLiteral("{{env.term}}")},
        {QStringLiteral("spaced"), QStringLiteral("{{ env.term }}")},
        {QStringLiteral("expression"), QStringLiteral("{{$.url.encode(\"a&b\")}}")}};

    EXPECT_EQ(qml::visiblePathWithQueryPairs(QStringLiteral("/search"), pairs),
              QStringLiteral("/search?delimiter=a%26b%3Dc&plus=a%2Bb&space=a%20b&template="
                             "{{env.term}}&spaced={{ env.term }}&expression="
                             "{{$.url.encode(\"a&b\")}}"));
}

TEST(RequestQuerySync, template_delimiters_do_not_split_the_visible_query) {
    const QString path{
        QStringLiteral("/search?q={{$.url.encode(\"a\\\"}}?b&c=d#e\")}}&next=2#results")};

    const auto pairs = qml::queryPairsFromVisiblePath(path);

    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].first, QStringLiteral("q"));
    EXPECT_EQ(pairs[0].second, QStringLiteral("{{$.url.encode(\"a\\\"}}?b&c=d#e\")}}"));
    EXPECT_EQ(pairs[1].first, QStringLiteral("next"));
    EXPECT_EQ(pairs[1].second, QStringLiteral("2"));
    EXPECT_EQ(qml::visiblePathWithQueryPairs(path, pairs), path);
}

TEST(RequestQuerySync, params_edit_preserves_untouched_raw_query_segments) {
    const QString path{QStringLiteral(
        "/search?flag&&plus=a+b&literal=%7B%7Bsecret.value%7D%7D&change=old&#fragment")};
    auto pairs = qml::queryPairsFromVisiblePath(path);
    ASSERT_EQ(pairs.size(), 4U);
    pairs[3].second = QStringLiteral("new");

    EXPECT_EQ(qml::visiblePathWithUpdatedQueryPair(path, 3, pairs[3]),
              QStringLiteral(
                  "/search?flag&&plus=a+b&literal=%7B%7Bsecret.value%7D%7D&change=new&#fragment"));
}

TEST(RequestQuerySync, new_blank_param_activates_template_during_successive_edits) {
    QString path{QStringLiteral("/search?value=")};
    std::pair<QString, QString> pair{QStringLiteral("value"), QString{}};
    const std::array edits{
        std::pair{QStringLiteral("{"), QStringLiteral("/search?value={")},
        std::pair{QStringLiteral("{{"), QStringLiteral("/search?value={{")},
        std::pair{QStringLiteral("{{env.term"), QStringLiteral("/search?value={{env.term")},
        std::pair{QStringLiteral("{{env.term}}"), QStringLiteral("/search?value={{env.term}}")}};

    for (const auto& [value, expectedPath] : edits) {
        SCOPED_TRACE(value.toStdString());
        pair.second = value;
        path = qml::visiblePathWithUpdatedQueryPair(path, 0, pair);

        EXPECT_EQ(path, expectedPath);
    }
}

TEST(RequestQuerySync, editing_encoded_literal_does_not_activate_template_syntax) {
    const QString path{QStringLiteral("/search?literal=%7B%7Bsecret.value%7D%7D")};
    auto pairs = qml::queryPairsFromVisiblePath(path);
    ASSERT_EQ(pairs.size(), 1U);
    pairs[0].first = QStringLiteral("renamed");
    pairs[0].second = QStringLiteral("{{secret.other}}");

    EXPECT_EQ(qml::visiblePathWithUpdatedQueryPair(path, 0, pairs[0]),
              QStringLiteral("/search?renamed=%7B%7Bsecret.other%7D%7D"));
}

TEST(RequestQuerySync, removing_row_preserves_shifted_encoded_literal) {
    const QString path{QStringLiteral("/search?remove=1&literal=%7B%7Bsecret.value%7D%7D")};
    auto pairs = qml::queryPairsFromVisiblePath(path);
    ASSERT_EQ(pairs.size(), 2U);
    pairs.erase(pairs.begin());

    EXPECT_EQ(qml::visiblePathWithoutQueryPairs(path, 0, 1),
              QStringLiteral("/search?literal=%7B%7Bsecret.value%7D%7D"));
}

TEST(RequestQuerySync, removing_duplicate_row_keeps_escaped_literal_provenance) {
    const QString path{QStringLiteral("/search?x={{secret.value}}&x=%7B%7Bsecret.value%7D%7D")};

    EXPECT_EQ(qml::visiblePathWithoutQueryPairs(path, 0, 1),
              QStringLiteral("/search?x=%7B%7Bsecret.value%7D%7D"));
}

TEST(RequestQuerySync, removing_row_keeps_empty_segment_anchored) {
    const QString path{QStringLiteral("/search?a=1&&b=2")};

    EXPECT_EQ(qml::visiblePathWithoutQueryPairs(path, 0, 1), QStringLiteral("/search?&b=2"));
}

TEST(RequestQuerySync, saved_structured_params_append_to_existing_visible_query) {
    const std::vector<std::pair<QString, QString>> pairs{
        {QStringLiteral("param"), QStringLiteral("3")}, {QStringLiteral("paramtwo"), QString{}}};

    EXPECT_EQ(qml::visiblePathWithAppendedQueryPairs(QStringLiteral("/api/testing?raw=1#fragment"),
                                                     pairs),
              QStringLiteral("/api/testing?raw=1&param=3&paramtwo=#fragment"));
}

TEST(RequestQuerySync, appending_params_preserves_trailing_empty_query_segment) {
    const std::vector<std::pair<QString, QString>> pairs{
        {QStringLiteral("next"), QStringLiteral("2")}};

    EXPECT_EQ(qml::visiblePathWithAppendedQueryPairs(QStringLiteral("/search?raw=1&"), pairs),
              QStringLiteral("/search?raw=1&&next=2"));
}

}  // namespace reqloom::desktop::tests
