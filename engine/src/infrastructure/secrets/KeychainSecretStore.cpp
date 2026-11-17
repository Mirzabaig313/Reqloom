// KeychainSecretStore — OS-keychain-backed secret storage via QtKeychain.
//
// Gated on REQLOOM_HAS_QTKEYCHAIN: without it a placeholder is compiled so the engine still links
// (`read` finds nothing, `write`/`remove` fail with SecretAccessFailed); it must never be the
// production path.
#include "KeychainSecretStore.h"

#ifdef REQLOOM_HAS_QTKEYCHAIN

#include <qt6keychain/keychain.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QString>

#include <string>

namespace reqloom::engine {

namespace {

// All Reqloom secrets share one keychain service so they do not collide with other apps.
constexpr const char* kKeychainService = "com.reqloom.secrets";

// Runs an async QtKeychain job on a local event loop. False without a QCoreApplication, where
// loop.exec() would block forever (e.g. headless tests).
template <typename Job>
[[nodiscard]] bool runJobBlocking(Job& job) {
    if (QCoreApplication::instance() == nullptr) {
        return false;
    }
    QEventLoop loop;
    bool done = false;
    QObject::connect(&job, &Job::finished, &loop, [&loop, &done]() {
        done = true;
        loop.quit();
    });
    job.start();
    if (!done) {
        loop.exec();
    }
    return true;
}

[[nodiscard]] QString toQt(const std::string& s) {
    return QString::fromStdString(s);
}

}  // namespace

KeychainSecretStore::KeychainSecretStore() = default;
KeychainSecretStore::~KeychainSecretStore() = default;

bool KeychainSecretStore::backendAvailable() noexcept {
    return true;
}

std::expected<std::optional<std::string>, ReqloomError> KeychainSecretStore::read(
    const std::string& name) {
    QKeychain::ReadPasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    unreachable_.store(false, std::memory_order_release);
    if (!runJobBlocking(job)) {
        // No event loop to drive the job: the secret is absent and this read could not reach a
        // keychain, so the engine says no secret backend is available.
        unreachable_.store(true, std::memory_order_release);
        return std::optional<std::string>{};
    }

    if (job.error() == QKeychain::EntryNotFound) {
        return std::optional<std::string>{};
    }
    if (job.error() == QKeychain::NoBackendAvailable) {
        // A headless Linux machine with no Secret Service on its session bus.
        unreachable_.store(true, std::memory_order_release);
        return std::optional<std::string>{};
    }
    if (job.error() != QKeychain::NoError) {
        return std::unexpected(ReqloomError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return std::optional<std::string>{job.textData().toStdString()};
}

std::expected<void, ReqloomError> KeychainSecretStore::write(const std::string& name,
                                                             const std::string& value) {
    QKeychain::WritePasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    job.setTextData(toQt(value));
    if (!runJobBlocking(job)) {
        return std::unexpected(ReqloomError{ErrorCode::SecretAccessFailed,
                                            ErrorClass::Auth,
                                            "keychain write requires a running QCoreApplication"});
    }

    if (job.error() != QKeychain::NoError) {
        return std::unexpected(ReqloomError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return {};
}

std::expected<void, ReqloomError> KeychainSecretStore::remove(const std::string& name) {
    QKeychain::DeletePasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    if (!runJobBlocking(job)) {
        return std::unexpected(ReqloomError{ErrorCode::SecretAccessFailed,
                                            ErrorClass::Auth,
                                            "keychain delete requires a running QCoreApplication"});
    }

    // A missing entry is not an error for remove(): the key is absent either way.
    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        return std::unexpected(ReqloomError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return {};
}

}  // namespace reqloom::engine

#else  // REQLOOM_HAS_QTKEYCHAIN

#include <string_view>

namespace reqloom::engine {

namespace {

[[nodiscard]] ReqloomError noBackendError(std::string_view action) {
    return ReqloomError{ErrorCode::SecretAccessFailed,
                        ErrorClass::Auth,
                        "secret not " + std::string{action} +
                            ": this build has no OS keychain backend. Install a Reqloom build "
                            "with keychain support, or run the reqloom CLI with the secret in "
                            "the environment variable REQLOOM_SECRET_<NAME>."};
}

}  // namespace

KeychainSecretStore::KeychainSecretStore() = default;
KeychainSecretStore::~KeychainSecretStore() = default;

bool KeychainSecretStore::backendAvailable() noexcept {
    return false;
}

std::expected<std::optional<std::string>, ReqloomError> KeychainSecretStore::read(
    const std::string& /*name*/) {
    unreachable_.store(true, std::memory_order_release);
    return std::optional<std::string>{};
}

std::expected<void, ReqloomError> KeychainSecretStore::write(const std::string& /*name*/,
                                                             const std::string& /*value*/) {
    return std::unexpected(noBackendError("saved"));
}

std::expected<void, ReqloomError> KeychainSecretStore::remove(const std::string& /*name*/) {
    return std::unexpected(noBackendError("removed"));
}

}  // namespace reqloom::engine

#endif  // REQLOOM_HAS_QTKEYCHAIN
