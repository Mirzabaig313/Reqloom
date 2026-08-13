// Template-aware URL delimiter helpers for request editing and serialization.
#pragma once

#include <QtCore/QChar>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace reqloom::desktop::url_template {

/// Find a URL delimiter outside complete or in-progress `{{...}}` references.
[[nodiscard]] inline qsizetype findDelimiter(const QString& text,
                                             QChar delimiter,
                                             qsizetype from = 0) noexcept {
    bool inReference{};
    for (qsizetype index = from; index < text.size(); ++index) {
        if (index + 1 < text.size()) {
            const QStringView pair{text.constData() + index, 2};
            if (!inReference && pair == QStringView{u"{{"}) {
                inReference = true;
                ++index;
                continue;
            }
            if (inReference && pair == QStringView{u"}}"}) {
                inReference = false;
                ++index;
                continue;
            }
        }
        if (!inReference && text.at(index) == delimiter) {
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
        const qsizetype end{text.indexOf(QStringLiteral("}}"), start + 2)};
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
