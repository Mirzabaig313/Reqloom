// FileProjectTrustStore — see header.
#include "FileProjectTrustStore.h"

#include <reqloom/engine/Factories.h>

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/ProjectIdentity.h>

#include <nlohmann/json.hpp>

#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

/// The trust document as read: `damaged` when the file exists but is too big, unreadable,
/// unparseable or not a JSON object. A missing file is an empty, undamaged document.
struct LoadedDocument {
    json doc = json::object();
    bool damaged{false};
};

[[nodiscard]] LoadedDocument loadChecked(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec) {
        return {};
    }
    const auto size = fs::file_size(file, ec);
    if (ec || size > kMaxFileBytes) {
        return {json::object(), true};
    }
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return {json::object(), true};
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return {json::object(), true};
    }
    return {std::move(doc), false};
}

/// The trust document, or an empty object when the file is missing or damaged — never an error,
/// never a crash. Reads of a damaged file find nothing approved and nothing bound; writes refuse
/// it (`lockedUpdate`).
[[nodiscard]] json loadDocument(const fs::path& file) {
    return loadChecked(file).doc;
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

[[nodiscard]] ReqloomError approvalError(std::string detail) {
    return ReqloomError{ErrorCode::HostNotApproved, ErrorClass::Auth, std::move(detail)};
}

/// The approved hosts recorded for `id`; anything that is not a list of strings counts as none.
[[nodiscard]] std::vector<std::string> recordedHosts(const json& doc, const std::string& id) {
    std::vector<std::string> hosts{};
    const auto projects = doc.find("projects");
    if (projects == doc.end() || !projects->is_object()) {
        return hosts;
    }
    const auto entry = projects->find(id);
    if (entry == projects->end() || !entry->is_object()) {
        return hosts;
    }
    const auto list = entry->find("approved_hosts");
    if (list == entry->end() || !list->is_array()) {
        return hosts;
    }
    for (const auto& item : *list) {
        if (item.is_string()) {
            hosts.push_back(item.get<std::string>());
        }
    }
    std::ranges::sort(hosts);
    return hosts;
}

void setHosts(json& doc, const std::string& id, const std::vector<std::string>& hosts) {
    auto& entry = doc["projects"][id];
    entry["approved_hosts"] = hosts;
}

/// Approvals belong to a project id bound to one folder: refuse a project without an id or a
/// folder, and a project in another folder that claims a recorded id.
[[nodiscard]] std::expected<void, ReqloomError> checkApprovalOwner(ProjectTrustStore& trust,
                                                                   const Project& project) {
    if (project.id.empty()) {
        return std::unexpected(approvalError(
            "this project has no id, so no host can be approved for it. Add `id: " +
            generateProjectId() + "` to reqloom.yaml (any unique value without '/')."));
    }
    if (project.rootDir.empty()) {
        return std::unexpected(approvalError(
            "this project was not loaded from a folder, so no host can be approved for it"));
    }
    // Anything that stops the folder check from running refuses the approvals instead of skipping
    // the check.
    if (auto valid = validateProjectId(project.id); !valid) {
        return std::unexpected(approvalError(valid.error().detail));
    }
    std::error_code ec;
    (void)fs::weakly_canonical(project.rootDir, ec);
    if (ec) {
        return std::unexpected(approvalError("cannot resolve the project folder '" +
                                             toUtf8(project.rootDir) + "': " + ec.message()));
    }
    if (auto owner = trust.folderFor(project.id); !owner) {
        return std::unexpected(owner.error());
    }
    if (auto other = projectFolderConflict(trust, project)) {
        return std::unexpected(approvalError(
            "hosts approved for project id '" + project.id + "' belong to the folder '" +
            toUtf8(*other) + "', but this project is in '" + toUtf8(project.rootDir) +
            "', so they were not used. If you moved the project, confirm the move in the "
            "Reqloom app's Manage Secrets dialog; if this is a copy, give it its own `id:`."));
    }
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
    auto loaded = loadChecked(file);
    if (loaded.damaged) {
        // Rewriting it from scratch would drop every folder binding and the CLI setting.
        return std::unexpected(storeError(
            "'" + toUtf8(file) +
            "' is damaged (not a JSON object, unreadable or over 4 MiB), so Reqloom will not "
            "change it. Repair it, or move it aside to start over; until then no host is "
            "approved and the command line cannot approve any."));
    }
    json doc = std::move(loaded.doc);
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

std::expected<bool, ReqloomError> FileProjectTrustStore::isApproved(const Project& project,
                                                                    const std::string& hostKey) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    if (auto owner = checkApprovalOwner(*this, project); !owner) {
        return std::unexpected(owner.error());
    }
    const json doc = loadDocument(file_);
    if (!recordedFolder(doc, project.id)) {
        return false;  // no record for the id: nothing was ever approved for it
    }
    const auto hosts = recordedHosts(doc, project.id);
    return std::ranges::find(hosts, hostKey) != hosts.end();
}

std::expected<void, ReqloomError> FileProjectTrustStore::approve(const Project& project,
                                                                 const std::string& hostKey) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    if (auto owner = checkApprovalOwner(*this, project); !owner) {
        return std::unexpected(owner.error());
    }
    // Binds the id to this folder when it has no record yet (trust on first use).
    if (auto bound = checkProjectFolder(this, project); !bound) {
        return std::unexpected(approvalError(bound.error().detail));
    }
    auto done = lockedUpdate(file_, [&](json& doc) {
        auto hosts = recordedHosts(doc, project.id);
        if (std::ranges::find(hosts, hostKey) == hosts.end()) {
            hosts.push_back(hostKey);
            std::ranges::sort(hosts);
        }
        setHosts(doc, project.id, hosts);
        return true;
    });
    if (!done) {
        return std::unexpected(done.error());
    }
    return {};
}

std::expected<void, ReqloomError> FileProjectTrustStore::revoke(const Project& project,
                                                                const std::string& hostKey) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    if (auto owner = checkApprovalOwner(*this, project); !owner) {
        return std::unexpected(owner.error());
    }
    if (const auto hosts = recordedHosts(loadDocument(file_), project.id);
        std::ranges::find(hosts, hostKey) == hosts.end()) {
        return {};
    }
    auto done = lockedUpdate(file_, [&](json& doc) {
        auto hosts = recordedHosts(doc, project.id);
        std::erase(hosts, hostKey);
        setHosts(doc, project.id, hosts);
        return true;
    });
    if (!done) {
        return std::unexpected(done.error());
    }
    return {};
}

std::expected<std::vector<std::string>, ReqloomError> FileProjectTrustStore::approvedHosts(
    const Project& project) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    if (auto owner = checkApprovalOwner(*this, project); !owner) {
        return std::unexpected(owner.error());
    }
    const json doc = loadDocument(file_);
    if (!recordedFolder(doc, project.id)) {
        return std::vector<std::string>{};
    }
    return recordedHosts(doc, project.id);
}

std::expected<bool, ReqloomError> FileProjectTrustStore::cliApprovalAllowed() {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    const auto loaded = loadChecked(file_);
    if (loaded.damaged) {
        return false;  // a damaged file never reads as "allowed"
    }
    const json& doc = loaded.doc;
    const auto settings = doc.find("settings");
    if (settings == doc.end() || !settings->is_object()) {
        return true;
    }
    const auto value = settings->find("cli_host_approval");
    // Anything but an explicit `true` after the person turned it off stays off.
    return value == settings->end() || !value->is_boolean() ? value == settings->end()
                                                            : value->get<bool>();
}

std::expected<void, ReqloomError> FileProjectTrustStore::setCliApprovalAllowed(const bool allowed) {
    if (pathError_) {
        return std::unexpected(*pathError_);
    }
    auto done = lockedUpdate(file_, [&](json& doc) {
        auto& settings = doc["settings"];
        if (!settings.is_object()) {
            settings = json::object();
        }
        settings["cli_host_approval"] = allowed;
        return true;
    });
    if (!done) {
        return std::unexpected(done.error());
    }
    return {};
}

namespace {

std::mutex& testDirectoryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<fs::path>& testDirectory() {
    static std::optional<fs::path> directory;
    return directory;
}

#if !defined(_WIN32)
/// The user's home directory from the account database, not from $HOME, which any process can set
/// for the commands it runs.
[[nodiscard]] std::optional<fs::path> accountHome() {
    const long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size > 0 ? static_cast<std::size_t>(size) : 16384U);
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 ||
        found == nullptr || found->pw_dir == nullptr || *found->pw_dir == '\0') {
        return std::nullopt;
    }
    return fs::path{found->pw_dir};
}
#endif

}  // namespace

void setConfigDirectoryForTesting(std::optional<std::filesystem::path> directory) {
    const std::scoped_lock lock{testDirectoryMutex()};
    testDirectory() = std::move(directory);
}

std::optional<std::filesystem::path> configDirectoryForTesting() {
    const std::scoped_lock lock{testDirectoryMutex()};
    return testDirectory();
}

std::expected<std::filesystem::path, ReqloomError> FileProjectTrustStore::defaultPath() {
    if (auto directory = configDirectoryForTesting()) {
        return *directory / "projects.json";
    }
    // No environment variable moves this file: it holds the approvals and the setting that stop a
    // project, or a program running commands, from approving hosts, so `REQLOOM_CONFIG_DIR`,
    // `$HOME` and `$XDG_CONFIG_HOME` are not consulted.
    fs::path dir;
#if defined(_WIN32)
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) {
        return std::unexpected(
            storeError("cannot find the per-user config directory: APPDATA is not set"));
    }
    dir = fromQt(appData) / "Reqloom";
#else
    const auto home = accountHome();
    if (!home) {
        return std::unexpected(
            storeError("cannot find the home directory of the current user account"));
    }
#if defined(__APPLE__)
    dir = *home / "Library" / "Application Support" / "Reqloom";
#else
    dir = *home / ".config" / "reqloom";
#endif
#endif
    return dir / "projects.json";
}

}  // namespace reqloom::engine
