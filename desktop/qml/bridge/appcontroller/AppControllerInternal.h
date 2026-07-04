// AppController file-local helpers, shared across the AppController*.cpp units.
// These were file-scope helpers in AppController.cpp; extracted so the class's
// method definitions can be split across translation units without duplicating
// them. Inline (not anonymous) so unused helpers in a given unit don't warn.
#pragma once

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/Predicate.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLatin1String>
#include <QtCore/QString>

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
