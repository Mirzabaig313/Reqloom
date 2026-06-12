// BodyHighlighter — see header. Rule-based highlighter attached to a QML
// TextArea document.
#include "BodyHighlighter.h"

#include <QtQuick/QQuickTextDocument>
#include <QtGui/QSyntaxHighlighter>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextDocument>
#include <QtCore/QRegularExpression>

#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

// Single-line regex rule: every match of `re` colours capture group `group`
// with `fmt`. Single-line is sufficient for JSON/XML/YAML tokens, which never
// span lines; block comments / multi-line strings are out of scope for the MVP.
class HighlighterImpl : public QSyntaxHighlighter {
public:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat fmt;
        int group{0};
    };

    using QSyntaxHighlighter::QSyntaxHighlighter;

    void setRules(std::vector<Rule> rules) {
        rules_ = std::move(rules);
        rehighlight();
    }

protected:
    void highlightBlock(const QString& text) override {
        for (const Rule& rule : rules_) {
            auto it = rule.re.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                const int start = match.capturedStart(rule.group);
                const int length = match.capturedLength(rule.group);
                if (start >= 0 && length > 0) {
                    setFormat(start, length, rule.fmt);
                }
            }
        }
    }

private:
    std::vector<Rule> rules_;
};

namespace {

[[nodiscard]] QTextCharFormat colorFormat(const QColor& color) {
    QTextCharFormat fmt;
    fmt.setForeground(color);
    return fmt;
}

[[nodiscard]] HighlighterImpl::Rule
rule(const QString& pattern, const QColor& color, int group = 0) {
    return HighlighterImpl::Rule{QRegularExpression(pattern), colorFormat(color), group};
}

}  // namespace

BodyHighlighter::BodyHighlighter(QObject* parent)
    : QObject(parent), impl_(std::make_unique<HighlighterImpl>(static_cast<QTextDocument*>(nullptr))) {}

BodyHighlighter::~BodyHighlighter() = default;

void BodyHighlighter::setDocument(QQuickTextDocument* doc) {
    if (doc == quickDoc_) {
        return;
    }
    quickDoc_ = doc;
    impl_->setDocument(doc != nullptr ? doc->textDocument() : nullptr);
    rebuild();
    emit documentChanged();
}

void BodyHighlighter::setLanguage(const QString& lang) {
    const QString normalized = lang.trimmed().toLower();
    if (normalized == language_) {
        return;
    }
    language_ = normalized;
    rebuild();
    emit languageChanged();
}

void BodyHighlighter::setPropertyColor(const QColor& c) {
    if (c == propertyColor_) {
        return;
    }
    propertyColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setStringColor(const QColor& c) {
    if (c == stringColor_) {
        return;
    }
    stringColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setNumberColor(const QColor& c) {
    if (c == numberColor_) {
        return;
    }
    numberColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setKeywordColor(const QColor& c) {
    if (c == keywordColor_) {
        return;
    }
    keywordColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setCommentColor(const QColor& c) {
    if (c == commentColor_) {
        return;
    }
    commentColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setPunctuationColor(const QColor& c) {
    if (c == punctuationColor_) {
        return;
    }
    punctuationColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::setTagColor(const QColor& c) {
    if (c == tagColor_) {
        return;
    }
    tagColor_ = c;
    rebuild();
    emit paletteChanged();
}

void BodyHighlighter::rebuild() {
    std::vector<HighlighterImpl::Rule> rules;
    const QString& lang = language_;

    if (lang == QLatin1String("json")) {
        // Value strings first; the key rule runs last so a "key": is recoloured
        // as a property even though it also matched the string rule.
        rules.push_back(rule(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""), stringColor_));
        rules.push_back(
            rule(QStringLiteral("-?\\b\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b"), numberColor_));
        rules.push_back(rule(QStringLiteral("\\b(?:true|false|null)\\b"), keywordColor_));
        rules.push_back(rule(QStringLiteral("[{}\\[\\],:]"), punctuationColor_));
        rules.push_back(rule(QStringLiteral("(\"(?:[^\"\\\\]|\\\\.)*\")\\s*:"), propertyColor_, 1));
    } else if (lang == QLatin1String("xml") || lang == QLatin1String("html")) {
        rules.push_back(rule(QStringLiteral("\"[^\"]*\""), stringColor_));
        rules.push_back(rule(QStringLiteral("</?\\s*([A-Za-z][\\w:-]*)"), tagColor_, 1));
        rules.push_back(rule(QStringLiteral("([A-Za-z_:][\\w:.-]*)\\s*="), propertyColor_, 1));
        rules.push_back(rule(QStringLiteral("<!--.*-->"), commentColor_));
    } else if (lang == QLatin1String("yaml")) {
        rules.push_back(rule(QStringLiteral("^\\s*([\\w.\\-]+)\\s*:"), propertyColor_, 1));
        rules.push_back(rule(QStringLiteral("\"[^\"]*\"|'[^']*'"), stringColor_));
        rules.push_back(
            rule(QStringLiteral("\\b-?\\d+(?:\\.\\d+)?\\b"), numberColor_));
        rules.push_back(
            rule(QStringLiteral("\\b(?:true|false|null|yes|no)\\b"), keywordColor_));
        rules.push_back(rule(QStringLiteral("#.*$"), commentColor_));
    } else if (lang == QLatin1String("javascript") || lang == QLatin1String("js")) {
        rules.push_back(rule(QStringLiteral("\"[^\"]*\"|'[^']*'|`[^`]*`"), stringColor_));
        rules.push_back(rule(
            QStringLiteral("\\b(?:function|return|const|let|var|if|else|for|while|new|class|"
                           "async|await|true|false|null|undefined|import|export|from|of|in)\\b"),
            keywordColor_));
        rules.push_back(
            rule(QStringLiteral("\\b-?\\d+(?:\\.\\d+)?\\b"), numberColor_));
        rules.push_back(rule(QStringLiteral("//.*$"), commentColor_));
    } else if (lang == QLatin1String("markdown") || lang == QLatin1String("md")) {
        rules.push_back(rule(QStringLiteral("^#{1,6}\\s.*$"), keywordColor_));
        rules.push_back(rule(QStringLiteral("\\*\\*[^*]+\\*\\*"), propertyColor_));
        rules.push_back(rule(QStringLiteral("`[^`]+`"), stringColor_));
    }
    // "text" / unknown → no rules (plain mono).

    impl_->setRules(std::move(rules));
}

}  // namespace reqloom::desktop::qml
