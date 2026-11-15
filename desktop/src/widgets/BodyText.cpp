// BodyText — see header.
#include "BodyText.h"

#include <nlohmann/json.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextFormat>
#include <QtGui/QTextFrame>
#include <QtGui/QTextTable>

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::desktop::widgets::body {

namespace {

constexpr std::size_t kIndentWidth = 2;

[[nodiscard]] bool isJsonSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isJsonSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isJsonSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool isJson(std::string_view text) {
    return nlohmann::json::accept(text.begin(), text.end());
}

void newline(std::string& out, std::size_t depth) {
    out.push_back('\n');
    out.append(depth * kIndentWidth, ' ');
}

// Re-indent text already known to be valid JSON, copying string and number tokens untouched.
[[nodiscard]] std::string reindent(std::string_view json) {
    if (json.starts_with("\xEF\xBB\xBF")) {
        json.remove_prefix(3);
    }
    std::string out;
    out.reserve(json.size() + json.size() / 2);
    std::size_t depth = 0;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            const std::size_t start = i;
            for (++i; json[i] != '"'; ++i) {
                if (json[i] == '\\') {
                    ++i;
                }
            }
            out.append(json.substr(start, i - start + 1));
        } else if (c == '{' || c == '[') {
            std::size_t next = i + 1;
            while (next < json.size() && isJsonSpace(json[next])) {
                ++next;
            }
            out.push_back(c);
            if (json[next] == (c == '{' ? '}' : ']')) {
                out.push_back(json[next]);
                i = next;
            } else {
                newline(out, ++depth);
            }
        } else if (c == '}' || c == ']') {
            newline(out, --depth);
            out.push_back(c);
        } else if (c == ',') {
            out.push_back(',');
            newline(out, depth);
        } else if (c == ':') {
            out.append(": ");
        } else if (!isJsonSpace(c)) {
            out.push_back(c);
        }
    }
    return out;
}

class InertDocument final : public QTextDocument {
protected:
    // Nothing a document names is fetched or read from disk: no images, no style sheets.
    QVariant loadResource(int type, const QUrl& name) override {
        Q_UNUSED(type);
        Q_UNUSED(name);
        return {};
    }
};

template <typename Format>
[[nodiscard]] bool dropBackgroundImage(Format& format) {
    if (!format.hasProperty(QTextFormat::BackgroundImageUrl)) {
        return false;
    }
    format.clearProperty(QTextFormat::BackgroundImageUrl);
    return true;
}

void stripFrameBackgrounds(QTextFrame* frame) {
    QTextFrameFormat frameFormat = frame->frameFormat();
    if (dropBackgroundImage(frameFormat)) {
        frame->setFrameFormat(frameFormat);
    }
    if (auto* table = qobject_cast<QTextTable*>(frame); table != nullptr) {
        for (int row = 0; row < table->rows(); ++row) {
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(row, column);
                QTextCharFormat cellFormat = cell.format();
                if (dropBackgroundImage(cellFormat)) {
                    cell.setFormat(cellFormat);
                }
            }
        }
    }
    for (QTextFrame* child : frame->childFrames()) {
        stripFrameBackgrounds(child);
    }
}

struct Span {
    int position{0};
    int length{0};
    bool image{false};
    QTextCharFormat format;
};

// Replace images with a placeholder and drop background images from blocks and characters.
void stripInlineResources(QTextDocument& doc) {
    std::vector<Span> spans;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat blockFormat = block.blockFormat();
        if (dropBackgroundImage(blockFormat)) {
            QTextCursor(block).setBlockFormat(blockFormat);
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            QTextCharFormat format = fragment.charFormat();
            const bool image = format.isImageFormat();
            if (image || dropBackgroundImage(format)) {
                spans.push_back(Span{fragment.position(), fragment.length(), image, format});
            }
        }
    }
    for (const Span& span : std::views::reverse(spans)) {
        QTextCursor cursor(&doc);
        cursor.setPosition(span.position);
        cursor.setPosition(span.position + span.length, QTextCursor::KeepAnchor);
        if (span.image) {
            cursor.insertText(QStringLiteral("[image]"), QTextCharFormat{});
        } else {
            cursor.setCharFormat(span.format);
        }
    }
}

}  // namespace

QString prettyJson(const QString& text, bool unwrapEncoded) {
    const QByteArray utf8 = text.toUtf8();
    const std::string_view json{utf8.constData(), static_cast<std::size_t>(utf8.size())};
    if (!isJson(json)) {
        return text;
    }
    if (unwrapEncoded && trimmed(json).starts_with('"')) {
        const auto inner = nlohmann::json::parse(json.begin(), json.end()).get<std::string>();
        const std::string_view innerJson = trimmed(inner);
        if ((innerJson.starts_with('{') || innerJson.starts_with('[')) && isJson(innerJson)) {
            return QString::fromStdString(reindent(innerJson));
        }
    }
    return QString::fromStdString(reindent(json));
}

QString inertRichText(const QString& source, bool markdown) {
    InertDocument doc;
    if (markdown) {
        doc.setMarkdown(source);
    } else {
        doc.setHtml(source);
    }
    stripFrameBackgrounds(doc.rootFrame());
    stripInlineResources(doc);
    return doc.toHtml();
}

}  // namespace reqloom::desktop::widgets::body
