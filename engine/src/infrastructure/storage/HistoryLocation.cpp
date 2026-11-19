// HistoryLocation — see header.
#include "HistoryLocation.h"

#include <reqloom/engine/Factories.h>

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QtEnvironmentVariables>

#include <string>

namespace reqloom::engine {

namespace {

[[nodiscard]] QString toQt(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] std::filesystem::path fromQt(const QString& path) {
#if defined(_WIN32)
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

/// The desktop app's `AppDataLocation`, computed without depending on this process's
/// application name (the CLI's is "reqloom").
[[nodiscard]] QString appDataDirectory() {
#if defined(_WIN32)
    const QString base = qEnvironmentVariable("APPDATA");
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    if (base.isEmpty()) {
        return {};
    }
    return QDir{base}.filePath(QStringLiteral("Reqloom/Reqloom"));
}

}  // namespace

std::expected<std::filesystem::path, ReqloomError> historyDatabasePathFor(
    const std::filesystem::path& projectRoot) {
    QString base;
    if (const auto testDir = configDirectoryForTesting()) {
        base = toQt(*testDir);
    } else {
        base = appDataDirectory();
    }
    if (base.isEmpty()) {
        return std::unexpected(ReqloomError{ErrorCode::Internal,
                                            ErrorClass::Run,
                                            "history: cannot find the per-user data directory"});
    }
    // Hash the canonical path for a stable, filesystem-safe name that does not reveal the path.
    const QString root = toQt(projectRoot);
    const QString canonical = QFileInfo(root).canonicalFilePath();
    const QString key = canonical.isEmpty() ? root : canonical;
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir{base}.filePath(QStringLiteral("history"));
    return fromQt(QDir{dir}.filePath(QStringLiteral("history-%1.db").arg(digest)));
}

}  // namespace reqloom::engine
