// Crypto — OpenSSL 3.x EVP APIs for HMAC and hashes.
#include "Crypto.h"

#include "../../domain/Codecs.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

#include <openssl/evp.h>
#include <openssl/hmac.h>

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <climits>
#include <cstring>
#include <string>
#include <string_view>

namespace chainapi::engine::crypto {

namespace {

/// HMAC via the legacy convenience wrapper. `HMAC()` is documented to
/// remain in OpenSSL 3.x and is simpler than EVP_MAC.
std::string hmacWith(const EVP_MD* md, std::string_view key, std::string_view data) {
    if (md == nullptr) return {};

    unsigned int outLen = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    // Cap keyLen defensively — no real key is anywhere near INT_MAX, but
    // the cast is UB on overflow.
    const auto keyLen = static_cast<int>(std::min(key.size(), static_cast<std::size_t>(INT_MAX)));
    const auto* result = HMAC(md,
                              key.data(),
                              keyLen,
                              reinterpret_cast<const unsigned char*>(data.data()),
                              data.size(),
                              buf.data(),
                              &outLen);
    if (result == nullptr || outLen == 0) return {};
    return std::string(reinterpret_cast<const char*>(buf.data()), outLen);
}

/// One-shot SHA via EVP_Digest. Returns empty on the (vanishingly unlikely)
/// OpenSSL failure path.
std::string hashWith(const EVP_MD* md, std::string_view data) {
    if (md == nullptr) return {};

    unsigned int outLen = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> buf{};
    if (EVP_Digest(data.data(), data.size(), buf.data(), &outLen, md, nullptr) != 1) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(buf.data()), outLen);
}

/// JWT base64url (RFC 7515 Appendix C): standard base64 with `+`→`-`,
/// `/`→`_`, and trailing `=` padding stripped.
std::string base64UrlEncode(std::string_view data) {
    std::string s = codecs::base64Encode(data);
    for (auto& c : s) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

/// JWT signature flow shared by HS256 and HS512.
std::string jwtSignHs(std::string_view payloadJson,
                      std::string_view key,
                      const EVP_MD* md,
                      std::string_view algName) {
    // Stable header order aids predictability across implementations even
    // though JWT itself is order-insensitive.
    std::string header = R"({"alg":")";
    header.append(algName);
    header.append(R"(","typ":"JWT"})");

    std::string signingInput = base64UrlEncode(header);
    signingInput.push_back('.');
    signingInput.append(base64UrlEncode(payloadJson));

    const auto signature = hmacWith(md, key, signingInput);
    if (signature.empty()) return {};

    signingInput.push_back('.');
    signingInput.append(base64UrlEncode(signature));
    return signingInput;
}

}  // namespace

std::string hmacSha1(std::string_view key, std::string_view data) {
    return hmacWith(EVP_sha1(), key, data);
}

std::string hmacSha256(std::string_view key, std::string_view data) {
    return hmacWith(EVP_sha256(), key, data);
}

std::string hmacSha512(std::string_view key, std::string_view data) {
    return hmacWith(EVP_sha512(), key, data);
}

std::string sha256(std::string_view data) {
    return hashWith(EVP_sha256(), data);
}

std::string jwtSignHs256(std::string_view payloadJson, std::string_view key) {
    return jwtSignHs(payloadJson, key, EVP_sha256(), "HS256");
}

std::string jwtSignHs512(std::string_view payloadJson, std::string_view key) {
    return jwtSignHs(payloadJson, key, EVP_sha512(), "HS512");
}

}  // namespace chainapi::engine::crypto
