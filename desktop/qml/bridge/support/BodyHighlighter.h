// BodyHighlighter — attaches a rule-based QSyntaxHighlighter to a QML
// TextArea's text document so request / response bodies are syntax-coloured by
// language (JSON, XML, HTML, YAML, JavaScript, Markdown). Colours are driven
// from QML (DesignTokens) so highlighting tracks the active theme. Presentation
// helper only; no engine state.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtQuick/QQuickTextDocument>

#include <memory>

namespace reqloom::desktop::qml {

class HighlighterImpl;

class BodyHighlighter : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickTextDocument* document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // Token colours — set from QML (DesignTokens) so the palette follows theme.
    Q_PROPERTY(QColor propertyColor READ propertyColor WRITE setPropertyColor NOTIFY paletteChanged)
    Q_PROPERTY(QColor stringColor READ stringColor WRITE setStringColor NOTIFY paletteChanged)
    Q_PROPERTY(QColor numberColor READ numberColor WRITE setNumberColor NOTIFY paletteChanged)
    Q_PROPERTY(QColor keywordColor READ keywordColor WRITE setKeywordColor NOTIFY paletteChanged)
    Q_PROPERTY(QColor commentColor READ commentColor WRITE setCommentColor NOTIFY paletteChanged)
    Q_PROPERTY(QColor punctuationColor READ punctuationColor WRITE setPunctuationColor NOTIFY
                   paletteChanged)
    Q_PROPERTY(QColor tagColor READ tagColor WRITE setTagColor NOTIFY paletteChanged)

public:
    explicit BodyHighlighter(QObject* parent = nullptr);
    ~BodyHighlighter() override;

    BodyHighlighter(const BodyHighlighter&) = delete;
    BodyHighlighter& operator=(const BodyHighlighter&) = delete;
    BodyHighlighter(BodyHighlighter&&) = delete;
    BodyHighlighter& operator=(BodyHighlighter&&) = delete;

    [[nodiscard]] QQuickTextDocument* document() const { return quickDoc_; }
    void setDocument(QQuickTextDocument* doc);

    [[nodiscard]] QString language() const { return language_; }
    void setLanguage(const QString& lang);

    [[nodiscard]] QColor propertyColor() const { return propertyColor_; }
    [[nodiscard]] QColor stringColor() const { return stringColor_; }
    [[nodiscard]] QColor numberColor() const { return numberColor_; }
    [[nodiscard]] QColor keywordColor() const { return keywordColor_; }
    [[nodiscard]] QColor commentColor() const { return commentColor_; }
    [[nodiscard]] QColor punctuationColor() const { return punctuationColor_; }
    [[nodiscard]] QColor tagColor() const { return tagColor_; }

    void setPropertyColor(const QColor& c);
    void setStringColor(const QColor& c);
    void setNumberColor(const QColor& c);
    void setKeywordColor(const QColor& c);
    void setCommentColor(const QColor& c);
    void setPunctuationColor(const QColor& c);
    void setTagColor(const QColor& c);

signals:
    void documentChanged();
    void languageChanged();
    void paletteChanged();

private:
    /// Rebuild the highlighting rules for the current language + palette and
    /// re-highlight the attached document.
    void rebuild();

    QQuickTextDocument* quickDoc_{nullptr};
    QString language_{QStringLiteral("text")};
    QColor propertyColor_{QStringLiteral("#7aa2f7")};
    QColor stringColor_{QStringLiteral("#9ece6a")};
    QColor numberColor_{QStringLiteral("#e0af68")};
    QColor keywordColor_{QStringLiteral("#bb9af7")};
    QColor commentColor_{QStringLiteral("#565f89")};
    QColor punctuationColor_{QStringLiteral("#a9b1d6")};
    QColor tagColor_{QStringLiteral("#7aa2f7")};
    std::unique_ptr<HighlighterImpl> impl_;
};

}  // namespace reqloom::desktop::qml
