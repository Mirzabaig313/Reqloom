// The desktop app and the CLI open the same run-history file for a project (PRD FR-16.3). The app
// used to compute the path itself (AppDataLocation/history/history-<sha1>.db); it now asks the
// engine, which must give the very same file, or existing users' history would disappear.

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>

#include <filesystem>
#include <optional>

namespace {

/// The formula `AppController::openProjectHistory` used before the engine named the file.
[[nodiscard]] QString formerDesktopPath(const QString& projectRoot) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString canonical = QFileInfo(projectRoot).canonicalFilePath();
    const QString key = canonical.isEmpty() ? projectRoot : canonical;
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir{base}.filePath(QStringLiteral("history"));
    return QDir{dir}.filePath(QStringLiteral("history-%1.db").arg(digest));
}

}  // namespace

TEST(HistoryPath, engine_path_is_the_file_the_desktop_app_always_used) {
    // Compute paths only: nothing is created or opened.
    const auto savedOverride = reqloom::engine::configDirectoryForTesting();
    const QString savedOrg = QCoreApplication::organizationName();
    const QString savedApp = QCoreApplication::applicationName();
    reqloom::engine::setConfigDirectoryForTesting(std::nullopt);
    QCoreApplication::setOrganizationName(QStringLiteral("Reqloom"));
    QCoreApplication::setApplicationName(QStringLiteral("Reqloom"));

    const auto root = std::filesystem::temp_directory_path();
    const QString rootText = QString::fromStdString(root.string());
    const auto enginePath = reqloom::engine::defaultHistoryDatabasePath(root);
    const QString expected = formerDesktopPath(rootText);

    QCoreApplication::setOrganizationName(savedOrg);
    QCoreApplication::setApplicationName(savedApp);
    reqloom::engine::setConfigDirectoryForTesting(savedOverride);

    ASSERT_TRUE(enginePath.has_value()) << enginePath.error().detail;
    EXPECT_EQ(QDir::cleanPath(QString::fromStdString(enginePath->string())),
              QDir::cleanPath(expected));
}
