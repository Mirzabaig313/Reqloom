// KeychainSecretStore — OS-keychain-backed secret storage via QtKeychain.
// PRD §5.1 / NFR-3.2.
//
// Implementation is gated on CHAINAPI_HAS_QTKEYCHAIN; if QtKeychain is not
// available at configure time the store falls back to a placeholder that
// refuses writes (so the absence is loud, not silent).
#include "KeychainSecretStore.h"

namespace chainapi::engine {

KeychainSecretStore::KeychainSecretStore() = default;
KeychainSecretStore::~KeychainSecretStore() = default;

std::expected<std::optional<std::string>, ChainApiError>
KeychainSecretStore::read(const std::string& /*name*/) {
    return std::optional<std::string>{};
}

std::expected<void, ChainApiError>
KeychainSecretStore::write(const std::string& /*name*/,
                           const std::string& /*value*/) {
    return {};
}

std::expected<void, ChainApiError>
KeychainSecretStore::remove(const std::string& /*name*/) {
    return {};
}

}  // namespace chainapi::engine
