// Template-aware URL delimiter helpers for request editing and serialization.
#pragma once

#include <QtCore/QChar>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace reqloom::desktop::url_template {

/// Find the closing delimiter of a `{{...}}` reference, ignoring quoted content.
[[nodiscard]] inline qsizetype findTemplateEnd(const QString& text,
                                               qsizetype templateStart) noexcept {
    if (templateStart < 0 || templateStart >= text.size() || text.size() - templateStart < 2 ||
        text.at(templateStart) != QLatin1Char('{') ||
        text.at(templateStart + 1) != QLatin1Char('{')) {
        return -1;
    }

    QChar quote{};
    bool escaped{};
    for (qsizetype index = templateStart + 2; index < text.size(); ++index) {
        const QChar character{text.at(index)};
        if (!quote.isNull()) {
            if (escaped) {
                escaped = false;
            } else if (character == QLatin1Char('\\')) {
                escaped = true;
            } else if (character == quote) {
                quote = QChar{};
            }
            continue;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('\'')) {
            quote = character;
            continue;
        }
        if (character == QLatin1Char('}') && index + 1 < text.size() &&
            text.at(index + 1) == QLatin1Char('}')) {
            return index;
        }
    }
    return -1;
}

/// Find a URL delimiter outside complete or in-progress `{{...}}` references.
[[nodiscard]] inline qsizetype findDelimiter(const QString& text,
                                             QChar delimiter,
                                             qsizetype from = 0) noexcept {
    if (from < 0) {
        return -1;
    }
    for (qsizetype index = from; index < text.size(); ++index) {
        if (index + 1 < text.size() && text.at(index) == QLatin1Char('{') &&
            text.at(index + 1) == QLatin1Char('{')) {
            const qsizetype templateEnd{findTemplateEnd(text, index)};
            if (templateEnd < 0) {
                QChar quote{};
                bool escaped{};
                for (qsizetype bodyIndex = index + 2; bodyIndex < text.size(); ++bodyIndex) {
                    const QChar character{text.at(bodyIndex)};
                    if (!quote.isNull()) {
                        if (escaped) {
                            escaped = false;
                        } else if (character == QLatin1Char('\\')) {
                            escaped = true;
                        } else if (character == quote) {
                            quote = QChar{};
                        }
                    } else if (character == QLatin1Char('"') || character == QLatin1Char('\'')) {
                        quote = character;
                    } else if (character == delimiter) {
                        return bodyIndex;
                    }
                }
                return -1;
            }
            index = templateEnd + 1;
            continue;
        }
        if (text.at(index) == delimiter) {
            return index;
        }
    }
    return -1;
}

/// Split on a delimiter while preserving delimiters inside `{{...}}` references.
[[nodiscard]] inline QStringList splitOutsideTemplates(const QString& text, QChar delimiter) {
    QStringList parts{};
    qsizetype start{};
    while (start <= text.size()) {
        const qsizetype delimiterIndex{findDelimiter(text, delimiter, start)};
        if (delimiterIndex < 0) {
            parts.push_back(text.sliced(start));
            break;
        }
        parts.push_back(text.sliced(start, delimiterIndex - start));
        start = delimiterIndex + 1;
    }
    return parts;
}

/// Whether a component contains an explicit `$.url.encode (...)` reference.
[[nodiscard]] inline bool containsExplicitUrlEncode(const QString& text) {
    qsizetype offset{};
    while (offset < text.size()) {
        const qsizetype start{text.indexOf(QStringLiteral("{{"), offset)};
        if (start < 0) {
            return false;
        }
        const qsizetype end{findTemplateEnd(text, start)};
        if (end < 0) {
            return false;
        }
        const QString body{text.sliced(start + 2, end - start - 2).trimmed()};
        constexpr auto kFunction = "$.url.encode";
        if (body.startsWith(QLatin1StringView{kFunction})) {
            const QString tail{body.sliced(QLatin1StringView{kFunction}.size()).trimmed()};
            if (tail.startsWith(QLatin1Char('(')) && tail.endsWith(QLatin1Char(')'))) {
                return true;
            }
        }
        offset = end + 2;
    }
    return false;
}

}  // namespace reqloom::desktop::url_template
