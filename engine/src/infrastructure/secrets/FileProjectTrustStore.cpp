// FileProjectTrustStore — see header.
#include "FileProjectTrustStore.h"

#include <nlohmann/json.hpp>

#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kFormatVersion = 1;
constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{4} * 1024 * 1024;
constexpr int kLockTimeoutMs = 10'000;
constexpr int kStaleLockMs = 30'000;

[[nodiscard]] ReqloomError storeError(std::string detail) {
    return ReqloomError{
        ErrorCode::SecretAccessFailed, ErrorClass::Auth, "project trust store: " + detail};
}

[[nodiscard]] std::string toUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return {u8.begin(), u8.end()};
}

[[nodiscard]] fs::path fromUtf8(const std::string& s) {
    return fs::path{std::u8string{s.begin(), s.end()}};
}

[[nodiscard]] QString toQt(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

[[nodiscard]] fs::path fromQt(const QString& s) {
    return fs::path{s.toStdU16String()};
}

/// The trust document, or an empty object when the file is missing, too big,
/// unparseable or not a JSON object — never an error, never a crash.
[[nodiscard]] json loadDocument(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > kMaxFileBytes) {
        return json::object();
    }
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return json::object();
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : json::object();
}

[[nodiscard]] std::optional<fs::path> recordedFolder(const json& doc, const std::string& id) {
    const auto projects = doc.find("projects");
    if (projects == doc.end() || !projects->is_object()) {
        return std::nullopt;
    }
    const auto entry = projects->find(id);
    if (entry == projects->end() || !entry->is_object()) {
        return std::nullopt;
    }
    const auto folder = entry->find("folder");
    if (folder == entry->end() || !folder->is_string() || folder->get<std::string>().empty()) {
        return std::nullopt;
    }
    return fromUtf8(folder->get<std::string>());
}

void setFolder(json& doc, const std::string& id, const fs::path& folder) {
    if (!doc.contains("version") || !doc["version"].is_number_integer()) {
        doc["version"] = kFormatVersion;
    }
    auto& projects = doc["projects"];
    if (!projects.is_object()) {
        projects = json::object();
    }
    auto& entry = projects[id];
    if (!entry.is_object()) {
        entry = json::object();
    }
    entry["folder"] = toUtf8(folder);
}

[[nodiscard]] std::expected<void, ReqloomError> ensureDirectory(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        return {};
    }
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(storeError("cannot create '" + toUtf8(dir) + "': " + ec.message()));
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    return {};
}

[[nodiscard]] std::expected<void, ReqloomError> saveDocument(const fs::path& file,
                                                             const json& doc) {
    QSaveFile out{toQt(file)};
    if (!out.open(QIODevice::WriteOnly)) {
        return std::unexpected(
            storeError("cannot write '" + toUtf8(file) + "': " + out.errorString().toStdString()));
    }
    (void)out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const std::string text = doc.dump(2) + "\n";
    if (out.write(text.data(), static_cast<qint64>(text.size())) !=
            static_cast<qint64>(text.size()) ||
        !out.commit()) {
        return std::unexpected(
            storeError("cannot write '" + toUtf8(file) + "': " + out.errorString().toStdString()));
    }
    std::error_code ec;
    fs::permissions(
        file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return {};
}

/// Run `mutate(doc)` as one read-modify-write under the lock file, so two
/// processes updating the record cannot lose each other's change.
template <typename Mutate>
[[nodiscard]] auto lockedUpdate(const fs::path& file, Mutate mutate)
    -> std::expected<decltype(mutate(std::declval<json&>())), ReqloomError> {
    if (auto dir = ensureDirectory(file.parent_path()); !dir) {
        return std::unexpected(dir.error());
    }
    QLockFile lock{toQt(file) + QStringLiteral(".lock")};
    lock.setStaleLockTime(kStaleLockMs);
    if (!lock.tryLock(kLockTimeoutMs)) {
        return std::unexpected(
            storeError("'" + toUtf8(file) + "' is locked by another Reqloom process; try again"));
    }
    json doc = loadDocument(file);
    auto result = mutate(doc);
    if (auto saved = saveDocument(file, doc); !saved) {
        return std::unexpected(saved.error());
    }
    return result;
}

}  // namespace

FileProjectTrustStore::FileProjectTrustStore(std::filesystem::path file) : file_(std::move(file)) {}

FileProjectTrustStore::FileProjectTrustStore(ReqloomError error) : pathError_(std::move(error)) {}

FileProjectTrustStore::~FileProjectTrustStore() = default;

std::expected<std::optional<std::filesystem::path>, ReqloomError> FileProjectTrustStore::folderFor(
    const std::string& projectId) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    // Lock-free: writers replace the file atomically, so a reader sees the old or the new document,
    // never a torn one.
    return recordedFolder(loadDocument(file_), projectId);
}

std::expected<std::filesystem::path, ReqloomError> FileProjectTrustStore::recordIfAbsent(
    const std::string& projectId, const std::filesystem::path& folder) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    if (auto existing = recordedFolder(loadDocument(file_), projectId)) {
        return *existing;  // fast path: nothing to write
    }
    return lockedUpdate(file_, [&](json& doc) {
        if (auto existing = recordedFolder(doc, projectId)) {
            return *existing;  // another process recorded it first
        }
        setFolder(doc, projectId, folder);
        return folder;
    });
}

std::expected<void, ReqloomError> FileProjectTrustStore::rebind(
    const std::string& projectId, const std::filesystem::path& folder) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    auto done = lockedUpdate(file_, [&](json& doc) {
        setFolder(doc, projectId, folder);
        return true;
    });
    if (!done) {
        return std::unexpected(done.error());
    }
    return {};
}

std::expected<std::filesystem::path, ReqloomError> FileProjectTrustStore::defaultPath() {
    const auto unresolved = [](const char* variable) {
        return std::unexpected(storeError(
            std::string{"cannot find the per-user config directory: "} + variable + " is not set"));
    };
    if (const QString configDir = qEnvironmentVariable("REQLOOM_CONFIG_DIR");
        !configDir.isEmpty()) {
        return fromQt(configDir) / "projects.json";
    }
    fs::path dir;
#if defined(_WIN32)
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) {
        return unresolved("APPDATA");
    }
    dir = fromQt(appData) / "Reqloom";
#else
    const QString home = qEnvironmentVariable("HOME");
#if defined(__APPLE__)
    if (home.isEmpty()) {
        return unresolved("HOME");
    }
    dir = fromQt(home) / "Library" / "Application Support" / "Reqloom";
#else
    const fs::path xdg = fromQt(qEnvironmentVariable("XDG_CONFIG_HOME"));
    if (xdg.is_absolute()) {
        dir = xdg / "reqloom";
    } else if (!home.isEmpty()) {
        dir = fromQt(home) / ".config" / "reqloom";
    } else {
        return unresolved("HOME");
    }
#endif
#endif
    return dir / "projects.json";
}

}  // namespace reqloom::engine
