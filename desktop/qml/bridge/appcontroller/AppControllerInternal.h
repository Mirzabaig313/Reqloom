// AppController file-local helpers, shared across the AppController*.cpp units.
// These were file-scope helpers in AppController.cpp; extracted so the class's
// method definitions can be split across translation units without duplicating
// them. Inline (not anonymous) so unused helpers in a given unit don't warn.
#pragma once

#include "application/UrlTemplate.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/Predicate.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLatin1String>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

/// Locate the bundled sample project so first run is useful without a dialog.
/// Walks up from the executable directory (mirrors the Widgets App).
[[nodiscard]] inline QString locateSampleProject() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int hops = 0; hops < 8; ++hops) {
        const QString candidate = dir.filePath(QStringLiteral("samples/marketplace/reqloom.yaml"));
        if (QFileInfo::exists(candidate)) {
            return dir.filePath(QStringLiteral("samples/marketplace"));
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

/// When an OpenAPI import fails, sniff the file head for the common
/// non-OpenAPI shapes (Postman collection, Swagger 2.0) so the toast can
/// say something actionable instead of the engine's terse "found ''".
/// Returns an empty string when nothing recognizable is found, so the
/// caller falls back to the engine's own error detail.
[[nodiscard]] inline QString importFailureHint(const std::filesystem::path& spec) {
    std::ifstream in{spec, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string head(8192, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.find("schema.getpostman.com") != std::string::npos ||
        head.find("_postman_id") != std::string::npos) {
        // Postman is supported — but if the importer still failed, the export
        // is likely malformed or an unsupported schema version.
        return QStringLiteral(
            "This Postman collection couldn't be imported. Re-export it as Collection v2.1 "
            "and try again.");
    }
    if (head.find("\"swagger\"") != std::string::npos ||
        head.find("swagger:") != std::string::npos) {
        return QStringLiteral(
            "This Swagger 2.0 document couldn't be imported. Check that it's valid and has a "
            "non-empty `paths` section.");
    }
    return {};
}

/// Normalize a project directory path to a stable comparison key: strips a
/// trailing separator and resolves `.`/`..`/symlinks, so two spellings of the
/// same project (`/x/proj` and `/x/proj/`) collapse to one. `weakly_canonical`
/// keeps paths that no longer exist working.
[[nodiscard]] inline QString canonicalProjectPath(const QString& path) {
    if (path.isEmpty()) {
        return path;
    }
    std::error_code ec;
    const auto canon =
        std::filesystem::weakly_canonical(std::filesystem::path{path.toStdString()}, ec);
    return ec ? path : QString::fromStdString(canon.string());
}

/// Turn a project name into a filesystem-safe folder name: lowercase, runs of
/// non-alphanumeric characters collapse to a single '-', trimmed. Falls back to
/// "imported-project" when nothing usable remains.
[[nodiscard]] inline std::string projectFolderSlug(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool pendingDash = false;
    for (const char c : name) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pendingDash && !out.empty()) {
                out.push_back('-');
            }
            pendingDash = false;
            out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else {
            pendingDash = true;
        }
    }
    return out.empty() ? std::string{"imported-project"} : out;
}

[[nodiscard]] inline QString decodeQueryComponent(const QString& component) {
    return QUrl::fromPercentEncoding(component.toUtf8());
}

[[nodiscard]] inline bool isPreservableQueryTemplateBody(const QString& body) {
    return !body.isEmpty() && !body.contains(QLatin1Char('{')) && !body.contains(QLatin1Char('}'));
}

[[nodiscard]] inline QString encodeQueryComponent(const QString& component,
                                                  bool preserveTemplates = true) {
    if (!preserveTemplates) {
        return QString::fromLatin1(QUrl::toPercentEncoding(component));
    }

    QString encoded{};
    qsizetype offset{};
    while (offset < component.size()) {
        const qsizetype templateStart{component.indexOf(QStringLiteral("{{"), offset)};
        const qsizetype templateEnd{
            templateStart >= 0 ? component.indexOf(QStringLiteral("}}"), templateStart + 2) : -1};
        if (templateStart < 0 || templateEnd < 0) {
            encoded += QString::fromLatin1(QUrl::toPercentEncoding(component.sliced(offset)));
            break;
        }
        if (!isPreservableQueryTemplateBody(
                component.sliced(templateStart + 2, templateEnd - templateStart - 2))) {
            const qsizetype encodedLength{templateStart + 2 - offset};
            encoded += QString::fromLatin1(
                QUrl::toPercentEncoding(component.sliced(offset, encodedLength)));
            offset = templateStart + 2;
            continue;
        }

        encoded += QString::fromLatin1(
            QUrl::toPercentEncoding(component.sliced(offset, templateStart - offset)));
        const qsizetype templateLength{templateEnd + 2 - templateStart};
        encoded += component.sliced(templateStart, templateLength);
        offset = templateEnd + 2;
    }
    return encoded;
}

struct VisibleQuerySegment {
    QString rawKey{};
    QString rawValue{};
    QString key{};
    QString value{};
    bool hasEquals{};
    bool empty{};
};

[[nodiscard]] inline std::vector<VisibleQuerySegment> querySegmentsFromVisiblePath(
    const QString& visiblePath) {
    const qsizetype fragmentIndex{url_template::findDelimiter(visiblePath, QLatin1Char('#'))};
    const qsizetype queryIndex{url_template::findDelimiter(visiblePath, QLatin1Char('?'))};
    if (queryIndex < 0 || (fragmentIndex >= 0 && queryIndex > fragmentIndex)) {
        return {};
    }

    const qsizetype queryEnd{fragmentIndex >= 0 ? fragmentIndex : visiblePath.size()};
    const QString query{visiblePath.sliced(queryIndex + 1, queryEnd - queryIndex - 1)};
    if (query.isEmpty()) {
        return {};
    }
    std::vector<VisibleQuerySegment> segments{};
    for (const QString& rawSegment : url_template::splitOutsideTemplates(query, QLatin1Char('&'))) {
        VisibleQuerySegment segment{};
        segment.empty = rawSegment.isEmpty();
        const qsizetype equalsIndex{url_template::findDelimiter(rawSegment, QLatin1Char('='))};
        segment.hasEquals = equalsIndex >= 0;
        segment.rawKey = segment.hasEquals ? rawSegment.left(equalsIndex) : rawSegment;
        segment.rawValue = segment.hasEquals ? rawSegment.sliced(equalsIndex + 1) : QString{};
        segment.key = decodeQueryComponent(segment.rawKey);
        segment.value = decodeQueryComponent(segment.rawValue);
        segments.push_back(std::move(segment));
    }
    return segments;
}

[[nodiscard]] inline bool canPreserveTemplatesFromRaw(const QString& raw, const QString& decoded) {
    if (raw.contains(QStringLiteral("%7B"), Qt::CaseInsensitive) ||
        raw.contains(QStringLiteral("%7D"), Qt::CaseInsensitive)) {
        return false;
    }
    return raw.contains(QStringLiteral("{{")) || !decoded.contains(QStringLiteral("{{"));
}

[[nodiscard]] inline QString rawQuerySegment(const VisibleQuerySegment& segment) {
    return segment.hasEquals ? segment.rawKey + QLatin1Char('=') + segment.rawValue
                             : segment.rawKey;
}

[[nodiscard]] inline QString serializeQueryPair(const std::pair<QString, QString>& pair,
                                                const VisibleQuerySegment* original = nullptr) {
    const bool sameKey{original != nullptr && pair.first == original->key};
    const bool sameValue{original != nullptr && pair.second == original->value};
    const QString key{sameKey ? original->rawKey
                              : encodeQueryComponent(
                                    pair.first,
                                    original == nullptr || canPreserveTemplatesFromRaw(
                                                               original->rawKey, original->key))};
    const QString value{sameValue
                            ? original->rawValue
                            : encodeQueryComponent(
                                  pair.second,
                                  original == nullptr || canPreserveTemplatesFromRaw(
                                                             original->rawValue, original->value))};
    if (original != nullptr && !original->hasEquals && sameValue) {
        return key;
    }
    return key + QLatin1Char('=') + value;
}

[[nodiscard]] inline QString serializeQueryPairs(
    const std::vector<std::pair<QString, QString>>& pairs) {
    QString query{};
    for (const auto& pair : pairs) {
        if (!query.isEmpty()) {
            query += QLatin1Char('&');
        }
        query += serializeQueryPair(pair);
    }
    return query;
}

[[nodiscard]] inline std::vector<std::pair<QString, QString>> queryPairsFromVisiblePath(
    const QString& visiblePath) {
    std::vector<std::pair<QString, QString>> pairs{};
    for (const auto& segment : querySegmentsFromVisiblePath(visiblePath)) {
        if (!segment.empty) {
            pairs.emplace_back(segment.key, segment.value);
        }
    }
    return pairs;
}

inline void appendRawQuerySegment(QString& query, bool& hasOutputSegment, const QString& segment) {
    if (hasOutputSegment) {
        query += QLatin1Char('&');
    }
    query += segment;
    hasOutputSegment = true;
}

[[nodiscard]] inline QString visiblePathWithQueryPairs(
    const QString& visiblePath, const std::vector<std::pair<QString, QString>>& pairs) {
    const qsizetype fragmentIndex{url_template::findDelimiter(visiblePath, QLatin1Char('#'))};
    const qsizetype queryIndex{url_template::findDelimiter(visiblePath, QLatin1Char('?'))};
    const bool hasQuery{queryIndex >= 0 && (fragmentIndex < 0 || queryIndex < fragmentIndex)};
    const qsizetype baseEnd{hasQuery ? queryIndex
                                     : (fragmentIndex >= 0 ? fragmentIndex : visiblePath.size())};
    const QString base{visiblePath.left(baseEnd)};
    const QString fragment{fragmentIndex >= 0 ? visiblePath.sliced(fragmentIndex) : QString{}};
    const QString query{serializeQueryPairs(pairs)};
    return query.isEmpty() ? base + fragment : base + QLatin1Char('?') + query + fragment;
}

[[nodiscard]] inline QString visiblePathWithUpdatedQueryPair(
    const QString& visiblePath, std::size_t pairIndex, const std::pair<QString, QString>& pair) {
    const qsizetype fragmentIndex{url_template::findDelimiter(visiblePath, QLatin1Char('#'))};
    const qsizetype queryIndex{url_template::findDelimiter(visiblePath, QLatin1Char('?'))};
    const bool hasQuery{queryIndex >= 0 && (fragmentIndex < 0 || queryIndex < fragmentIndex)};
    const qsizetype baseEnd{hasQuery ? queryIndex
                                     : (fragmentIndex >= 0 ? fragmentIndex : visiblePath.size())};
    const QString base{visiblePath.left(baseEnd)};
    const QString fragment{fragmentIndex >= 0 ? visiblePath.sliced(fragmentIndex) : QString{}};

    QString query{};
    bool hasOutputSegment{};
    bool updated{};
    std::size_t currentPair{};
    for (const auto& segment : querySegmentsFromVisiblePath(visiblePath)) {
        if (segment.empty) {
            appendRawQuerySegment(query, hasOutputSegment, {});
        } else if (currentPair == pairIndex) {
            appendRawQuerySegment(query, hasOutputSegment, serializeQueryPair(pair, &segment));
            updated = true;
            ++currentPair;
        } else {
            appendRawQuerySegment(query, hasOutputSegment, rawQuerySegment(segment));
            ++currentPair;
        }
    }
    if (!updated) {
        appendRawQuerySegment(query, hasOutputSegment, serializeQueryPair(pair));
    }
    return base + QLatin1Char('?') + query + fragment;
}

[[nodiscard]] inline QString visiblePathWithoutQueryPairs(const QString& visiblePath,
                                                          std::size_t firstPair,
                                                          std::size_t count) {
    if (count == 0) {
        return visiblePath;
    }
    const qsizetype fragmentIndex{url_template::findDelimiter(visiblePath, QLatin1Char('#'))};
    const qsizetype queryIndex{url_template::findDelimiter(visiblePath, QLatin1Char('?'))};
    if (queryIndex < 0 || (fragmentIndex >= 0 && queryIndex > fragmentIndex)) {
        return visiblePath;
    }
    const QString base{visiblePath.left(queryIndex)};
    const QString fragment{fragmentIndex >= 0 ? visiblePath.sliced(fragmentIndex) : QString{}};

    QString query{};
    bool hasOutputSegment{};
    bool removed{};
    std::size_t currentPair{};
    for (const auto& segment : querySegmentsFromVisiblePath(visiblePath)) {
        const bool remove{!segment.empty && currentPair >= firstPair &&
                          currentPair - firstPair < count};
        if (!remove) {
            appendRawQuerySegment(query, hasOutputSegment, rawQuerySegment(segment));
        } else {
            removed = true;
        }
        if (!segment.empty) {
            ++currentPair;
        }
    }
    if (!removed) {
        return visiblePath;
    }
    return hasOutputSegment ? base + QLatin1Char('?') + query + fragment : base + fragment;
}

[[nodiscard]] inline QString visiblePathWithAppendedQueryPairs(
    const QString& visiblePath, const std::vector<std::pair<QString, QString>>& pairs) {
    const QString appendedQuery{serializeQueryPairs(pairs)};
    if (appendedQuery.isEmpty()) {
        return visiblePath;
    }

    const qsizetype fragmentIndex{url_template::findDelimiter(visiblePath, QLatin1Char('#'))};
    const QString prefix{fragmentIndex >= 0 ? visiblePath.left(fragmentIndex) : visiblePath};
    const QString fragment{fragmentIndex >= 0 ? visiblePath.sliced(fragmentIndex) : QString{}};
    if (url_template::findDelimiter(prefix, QLatin1Char('?')) < 0) {
        return prefix + QLatin1Char('?') + appendedQuery + fragment;
    }
    if (prefix.endsWith(QLatin1Char('?'))) {
        return prefix + appendedQuery + fragment;
    }
    if (prefix.endsWith(QLatin1Char('&'))) {
        return prefix + QLatin1Char('&') + appendedQuery + fragment;
    }
    return prefix + QLatin1Char('&') + appendedQuery + fragment;
}

[[nodiscard]] inline QString methodLabel(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

[[nodiscard]] inline engine::HttpMethod methodFromLabel(const QString& label) {
    const QString upper = label.trimmed().toUpper();
    if (upper == QLatin1String("POST")) {
        return engine::HttpMethod::Post;
    }
    if (upper == QLatin1String("PUT")) {
        return engine::HttpMethod::Put;
    }
    if (upper == QLatin1String("PATCH")) {
        return engine::HttpMethod::Patch;
    }
    if (upper == QLatin1String("DELETE")) {
        return engine::HttpMethod::Delete;
    }
    if (upper == QLatin1String("HEAD")) {
        return engine::HttpMethod::Head;
    }
    if (upper == QLatin1String("OPTIONS")) {
        return engine::HttpMethod::Options;
    }
    return engine::HttpMethod::Get;
}

/// Derive the extraction source from the path prefix, mirroring the engine's
/// YamlSchemaParser (and the old ExtractionTableEditor) so the in-memory
/// Extraction matches what a reload would produce.
[[nodiscard]] inline engine::Extraction::Source sourceForPath(const std::string& path) {
    if (path.starts_with("$.headers.")) {
        return engine::Extraction::Source::Header;
    }
    if (path.starts_with("$.cookies.")) {
        return engine::Extraction::Source::Cookie;
    }
    if (path == "$.status_code") {
        return engine::Extraction::Source::StatusCode;
    }
    return engine::Extraction::Source::JsonPath;
}

/// Mirror of ProjectModel's id-breaking guard for live dialog validation.
[[nodiscard]] inline bool hasIdBreakingChars(const QString& name) {
    return name.contains(QLatin1Char('.')) || name.contains(QLatin1Char('/')) ||
           name.contains(QLatin1Char('\\'));
}

/// Convert an ordered string map into QString pairs for an EditableKeyValueModel
/// (insertion/sort order preserved). Used to seed the edit controls from an op.
[[nodiscard]] inline std::vector<std::pair<QString, QString>> toEditPairs(
    const std::map<std::string, std::string>& pairs) {
    std::vector<std::pair<QString, QString>> out;
    out.reserve(pairs.size());
    for (const auto& [key, value] : pairs) {
        out.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
    }
    return out;
}

}  // namespace reqloom::desktop::qml
