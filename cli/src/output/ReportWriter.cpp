// Writes a finished report to `--output` or standard output, reporting every write failure.

#include "ReportWriter.h"

#include "../commands/PathText.h"

#include <QtCore/QSaveFile>
#include <QtCore/QString>

#include <cerrno>
#include <cstddef>
#include <format>
#include <system_error>

namespace reqloom::cli {

std::expected<void, std::string> writeReportFile(const std::filesystem::path& path,
                                                 std::string_view document) {
    const auto name = pathToUtf8(path);
    QSaveFile file(QString::fromStdU16String(path.u16string()));
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(
            std::format("cannot create '{}': {}", name, file.errorString().toStdString()));
    }
    const auto size = static_cast<qint64>(document.size());
    if (file.write(document.data(), size) != size || !file.commit()) {
        return std::unexpected(
            std::format("cannot write '{}': {}", name, file.errorString().toStdString()));
    }
    return {};
}

std::expected<void, std::string> writeReportStream(std::FILE* stream, std::string_view document) {
    errno = 0;
    const std::size_t written = std::fwrite(document.data(), 1, document.size(), stream);
    const bool flushed = std::fflush(stream) == 0;
    if (written != document.size() || !flushed || std::ferror(stream) != 0) {
        const int error = errno;
        return std::unexpected(error == 0 ? std::string{"write failed"}
                                          : std::generic_category().message(error));
    }
    return {};
}

}  // namespace reqloom::cli
