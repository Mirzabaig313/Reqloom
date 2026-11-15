// Body text transforms: token-preserving JSON formatting and the inert rich-text preview.
#include "widgets/BodyText.h"

#include <gtest/gtest.h>

#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QTextDocument>

namespace reqloom::desktop::widgets::body::tests {

namespace {

// Records every resource the rich-text engine asks for while parsing, laying out and painting.
class RecordingDocument final : public QTextDocument {
public:
    QStringList requested;

protected:
    QVariant loadResource(int type, const QUrl& name) override {
        Q_UNUSED(type);
        requested.append(name.toString());
        return {};
    }
};

[[nodiscard]] QStringList resourcesRequestedBy(const QString& html) {
    RecordingDocument doc;
    doc.setHtml(html);
    doc.setTextWidth(400);
    QImage canvas(400, 400, QImage::Format_ARGB32);
    QPainter painter(&canvas);
    doc.drawContents(&painter);
    return doc.requested;
}

const QString kHostileHtml = QStringLiteral(
    R"html(<html><head><link rel="stylesheet" href="http://evil.test/s.css">)html"
    R"html(<style>body { background-image: url(http://evil.test/css.png) }</style></head>)html"
    R"html(<body background="http://evil.test/body.png"><p>Hello )html"
    R"html(<img src="http://evil.test/pixel.png" width="1" height="1"> world</p>)html"
    R"html(<table background="http://evil.test/table.png"><tr>)html"
    R"html(<td background="http://evil.test/td.png" style="background-image: url(http://evil.test/td2.png)">)html"
    R"html(cell</td></tr></table>)html"
    R"html(<p style="background-image: url(http://evil.test/p.png)">para</p></body></html>)html");

}  // namespace

TEST(BodyText, pretty_json_keeps_number_tokens_exactly) {
    const QString pretty = prettyJson(QStringLiteral(R"({"a":9007199254740993,"b":[1.10,2E5]})"));

    EXPECT_EQ(
        pretty,
        QStringLiteral("{\n  \"a\": 9007199254740993,\n  \"b\": [\n    1.10,\n    2E5\n  ]\n}"));
}

TEST(BodyText, pretty_json_keeps_empty_containers_on_one_line) {
    EXPECT_EQ(prettyJson(QStringLiteral(R"( {"a":{ },"b":[ ]} )")),
              QStringLiteral("{\n  \"a\": {},\n  \"b\": []\n}"));
}

TEST(BodyText, pretty_json_copies_strings_with_structural_characters_verbatim) {
    EXPECT_EQ(prettyJson(QStringLiteral(R"({"s":"a, {[\"]}: \\ é"})")), QStringLiteral(R"({
  "s": "a, {[\"]}: \\ é"
})"));
}

TEST(BodyText, pretty_json_leaves_non_json_unchanged) {
    const QString templ = QStringLiteral(R"({"id": {{user.id}}})");
    EXPECT_EQ(prettyJson(templ), templ);
    EXPECT_EQ(prettyJson(QString{}), QString{});
    EXPECT_EQ(prettyJson(QStringLiteral("  \n")), QStringLiteral("  \n"));
}

TEST(BodyText, pretty_json_unwraps_an_encoded_document_only_when_asked) {
    const QString encoded = QStringLiteral(R"("{\"a\":12345678901234567890}")");

    EXPECT_EQ(prettyJson(encoded, true), QStringLiteral("{\n  \"a\": 12345678901234567890\n}"));
    EXPECT_EQ(prettyJson(encoded, false), encoded);
}

TEST(BodyText, hostile_html_requests_resources_before_it_is_made_inert) {
    const QStringList requested = resourcesRequestedBy(kHostileHtml);

    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/css.png")));
    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/pixel.png")));
    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/body.png")));
    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/td.png")));
    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/td2.png")));
    EXPECT_TRUE(requested.contains(QStringLiteral("http://evil.test/p.png")));
}

TEST(BodyText, inert_html_requests_no_resource_and_keeps_the_text) {
    const QString inert = inertRichText(kHostileHtml, false);

    EXPECT_TRUE(resourcesRequestedBy(inert).isEmpty()) << inert.toStdString();
    EXPECT_FALSE(inert.contains(QStringLiteral("evil.test")));
    EXPECT_TRUE(inert.contains(QStringLiteral("Hello")));
    EXPECT_TRUE(inert.contains(QStringLiteral("world")));
    EXPECT_TRUE(inert.contains(QStringLiteral("cell")));
    EXPECT_TRUE(inert.contains(QStringLiteral("[image]")));
}

TEST(BodyText, inert_markdown_requests_no_resource) {
    const QString markdown = QStringLiteral(
        "# Title\n\n![logo](http://evil.test/a.png)\n\n[docs](https://example.com/docs)\n\n"
        "<img src=\"http://evil.test/b.png\">\n\n![ref][r]\n\n[r]: http://evil.test/c.png\n");

    const QString inert = inertRichText(markdown, true);

    EXPECT_TRUE(resourcesRequestedBy(inert).isEmpty()) << inert.toStdString();
    EXPECT_FALSE(inert.contains(QStringLiteral("evil.test")));
    EXPECT_TRUE(inert.contains(QStringLiteral("Title")));
    EXPECT_TRUE(inert.contains(QStringLiteral("docs")));
}

}  // namespace reqloom::desktop::widgets::body::tests
