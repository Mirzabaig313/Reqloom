// KeychainSecretStore — OS-keychain-backed secret storage via QtKeychain.
//
// Gated on CHAINAPI_HAS_QTKEYCHAIN: when QtKeychain is found at configure
// time the real backend below is compiled (macOS Keychain, Windows
// Credential Store, libsecret/KWallet on Linux). Otherwise a no-op
// placeholder is compiled so the engine still links — `read` reports no
// secret, writes are dropped. The no-op path exists for build
// environments without QtKeychain; it must never be the production path.
#include "KeychainSecretStore.h"

#ifdef CHAINAPI_HAS_QTKEYCHAIN

#include <qt6keychain/keychain.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QString>

#include <string>

namespace chainapi::engine {

namespace {

// All ChainAPI secrets share one logical service so they're grouped in
// the OS keychain and don't collide with other apps' entries.
constexpr const char* kKeychainService = "com.chainapi.secrets";

template <typename Job>
void runJobBlocking(Job& job) {
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

std::expected<std::optional<std::string>, ChainApiError> KeychainSecretStore::read(
    const std::string& name) {
    QKeychain::ReadPasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    runJobBlocking(job);

    if (job.error() == QKeychain::EntryNotFound) {
        return std::optional<std::string>{};
    }
    if (job.error() != QKeychain::NoError) {
        return std::unexpected(ChainApiError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return std::optional<std::string>{job.textData().toStdString()};
}

std::expected<void, ChainApiError> KeychainSecretStore::write(const std::string& name,
                                                              const std::string& value) {
    QKeychain::WritePasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    job.setTextData(toQt(value));
    runJobBlocking(job);

    if (job.error() != QKeychain::NoError) {
        return std::unexpected(ChainApiError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return {};
}

std::expected<void, ChainApiError> KeychainSecretStore::remove(const std::string& name) {
    QKeychain::DeletePasswordJob job{QString::fromUtf8(kKeychainService)};
    job.setAutoDelete(false);
    job.setKey(toQt(name));
    runJobBlocking(job);

    // A missing entry is not an error for remove() — the post-condition
    // (the key is absent) already holds.
    if (job.error() != QKeychain::NoError && job.error() != QKeychain::EntryNotFound) {
        return std::unexpected(ChainApiError{
            ErrorCode::SecretAccessFailed, ErrorClass::Auth, job.errorString().toStdString()});
    }
    return {};
}

}  // namespace chainapi::engine

#else  // CHAINAPI_HAS_QTKEYCHAIN

namespace chainapi::engine {

KeychainSecretStore::KeychainSecretStore() = default;
KeychainSecretStore::~KeychainSecretStore() = default;

bool KeychainSecretStore::backendAvailable() noexcept {
    return false;
}

std::expected<std::optional<std::string>, ChainApiError> KeychainSecretStore::read(
    const std::string& /*name*/) {
    return std::optional<std::string>{};
}

std::expected<void, ChainApiError> KeychainSecretStore::write(const std::string& /*name*/,
                                                              const std::string& /*value*/) {
    return {};
}

std::expected<void, ChainApiError> KeychainSecretStore::remove(const std::string& /*name*/) {
    return {};
}

}  // namespace chainapi::engine

#endif  // CHAINAPI_HAS_QTKEYCHAIN
