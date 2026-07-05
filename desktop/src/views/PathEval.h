// PathEval — classify an extraction JSONPath against a response body for the
// chain editor's inline validity indicator + value preview. Pure (no Qt) so it
// is unit-testable; the Qt wrapper (AppController) only picks which body to use.
#pragma once

#include <reqloom/engine/JsonValues.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <string>
#include <string_view>

namespace reqloom::desktop::views {

enum class PathState { Neutral, Match, NoMatch };

struct PathEvalResult {
    PathState state{PathState::Neutral};
    std::string value;  ///< First matched value when state == Match.
};

/// Resolve `path` against `body`. Match → first value; valid body but no match
/// → NoMatch; nothing to check → Neutral.
[[nodiscard]] inline PathEvalResult classifyExtractionPath(std::string_view body,
                                                           std::string_view path) {
    if (path.empty() || body.empty()) {
        return {};
    }
    // ponytail: header/status/cookie paths aren't in the JSON body, so we can't
    // confirm them here — stay neutral rather than flag a false "no match".
    // Upgrade path: validate these against the step's captured response
    // headers/status when a run has produced them.
    if (path.starts_with("$.headers") || path.starts_with("$.cookies") || path == "$.status_code") {
        return {};
    }
    const std::vector<std::string> matches = engine::extractValues(std::string(body), path);
    if (matches.empty()) {
        return {PathState::NoMatch, {}};
    }
    return {PathState::Match, matches.front()};
}

namespace detail {
/// Recurse a JSON value, appending the `$.a.b[0]`-form path of every leaf
/// (scalar) — the addressable targets for an extraction.
inline void collectLeafPaths(const QJsonValue& value, const QString& path, QStringList& out) {
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            collectLeafPaths(it.value(), path + QStringLiteral(".") + it.key(), out);
        }
    } else if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            collectLeafPaths(arr.at(i), path + QStringLiteral("[%1]").arg(i), out);
        }
    } else {
        out.append(path);
    }
}
}  // namespace detail

/// Leaf JSONPaths in `body` whose path contains `filter` (case-insensitive;
/// empty = all), capped at `limit`. Powers the path field's autocomplete.
[[nodiscard]] inline QStringList collectJsonPaths(const QString& body,
                                                  const QString& filter,
                                                  int limit = 50) {
    QStringList all;
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8());
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            detail::collectLeafPaths(it.value(), QStringLiteral("$.") + it.key(), all);
        }
    } else if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (int i = 0; i < arr.size(); ++i) {
            detail::collectLeafPaths(arr.at(i), QStringLiteral("$[%1]").arg(i), all);
        }
    }
    QStringList out;
    for (const QString& p : all) {
        if (filter.isEmpty() || p.contains(filter, Qt::CaseInsensitive)) {
            out.append(p);
            if (out.size() >= limit) {
                break;
            }
        }
    }
    return out;
}

}  // namespace reqloom::desktop::views
