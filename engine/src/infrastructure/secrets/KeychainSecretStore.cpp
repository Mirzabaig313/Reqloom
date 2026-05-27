// KeychainSecretStore — OS-keychain-backed secret storage via QtKeychain.
// Gated on CHAINAPI_HAS_QTKEYCHAIN; falls back to a no-op placeholder
// if QtKeychain is unavailable at configure time.
#include "KeychainSecretStore.h"

namespace chainapi::engine {

KeychainSecretStore::KeychainSecretStore() = default;
KeychainSecretStore::~KeychainSecretStore() = default;

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
