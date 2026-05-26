// RequestSigners — see header. OAuth 1.0a signing per RFC 5849 §3.4.
#include "RequestSigners.h"

#include "../domain/Codecs.h"
#include "../infrastructure/util/Crypto.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine {

namespace {

using namespace codecs;

/// Generate an OAuth1 nonce (RFC 5849 §3.3 — "unique for the timestamp").
/// 64 hex chars from a thread-local PRNG.
///
/// std::random_device is non-deterministic on macOS/Linux/MSVC.
/// On MinGW it may be deterministic — replace with BCryptGenRandom if
/// Windows support via MinGW is added.
std::string randomNonce() {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::string nonce;
    nonce.reserve(64);
    for (int i = 0; i < 4; ++i) {
        const auto v = dist(gen);
        for (int j = 0; j < 8; ++j) {
            constexpr char hex[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned>((v >> (j * 8)) & 0xFFu);
            nonce.push_back(hex[byte >> 4]);
            nonce.push_back(hex[byte & 0xF]);
        }
    }
    return nonce;
}

std::string nowUnixSeconds() {
    using namespace std::chrono;
    const auto secs = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    return std::to_string(secs);
}

/// Split a URL into base (scheme://host[:port]/path) + query string.
/// RFC 5849 §3.4.1.2: the signature base URL excludes the query.
/// Returns nullopt when the URL is too malformed to find the scheme delimiter.
struct UrlParts {
    std::string base;        ///< scheme://host[:port]/path, no query/fragment
    std::string queryString; ///< everything after the first '?', no leading '?'
};

std::optional<UrlParts> splitUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return std::nullopt;

    const auto pathStart = url.find('/', schemeEnd + 3);
    const auto queryStart = url.find('?', schemeEnd + 3);
    const auto fragStart  = url.find('#', schemeEnd + 3);

    UrlParts out;
    const auto baseEnd = std::min(queryStart, fragStart);
    if (baseEnd == std::string_view::npos) {
        out.base = std::string{url};
    } else {
        out.base = std::string{url.substr(0, baseEnd)};
    }

    if (pathStart == std::string_view::npos ||
        (queryStart != std::string_view::npos && pathStart > queryStart) ||
        (fragStart  != std::string_view::npos && pathStart > fragStart)) {
        // No path component — append "/" per RFC 5849 §3.4.1.2 which
        // requires the path with "/" as the default for empty.
        out.base += '/';
    }

    if (queryStart != std::string_view::npos) {
        // qEnd is the fragment '#' position, if it comes AFTER the query '?'.
        // If '#' comes before '?' the URL is malformed; take the query string
        // to end-of-URL as the least-surprising fallback.
        const auto qEnd = (fragStart == std::string_view::npos ||
                           fragStart < queryStart)
            ? std::string_view::npos
            : fragStart;
        out.queryString = std::string{
            url.substr(queryStart + 1,
                       qEnd == std::string_view::npos
                           ? std::string_view::npos
                           : qEnd - queryStart - 1)};
    }
    return out;
}

/// Parse a query string into name/value pairs. Values are percent-decoded
/// so they can be re-encoded with the strict OAuth1 alphabet.
std::vector<std::pair<std::string, std::string>>
parseQuery(std::string_view qs) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < qs.size()) {
        const auto amp = qs.find('&', i);
        const auto piece = qs.substr(
            i, amp == std::string_view::npos ? std::string_view::npos
                                              : amp - i);
        const auto eq = piece.find('=');
        std::string name;
        std::string value;
        if (eq == std::string_view::npos) {
            name = std::string{piece};
        } else {
            name  = std::string{piece.substr(0, eq)};
            value = std::string{piece.substr(eq + 1)};
        }
        // Decode then re-encode in the canonicalisation step.
        // urlDecode returns nullopt for malformed %-escapes; keep the
        // encoded form as-is.
        if (auto d = codecs::urlDecode(name);  d) name  = std::move(*d);
        if (auto d = codecs::urlDecode(value); d) value = std::move(*d);
        out.emplace_back(std::move(name), std::move(value));
        if (amp == std::string_view::npos) break;
        i = amp + 1;
    }
    return out;
}

}  // namespace

bool signOAuth1Request(HttpRequest& req,
                       const ActorSession& session,
                       const OAuth1TestOverrides& overrides) {
    // Required: consumer_key + consumer_secret. Token + token_secret are
    // optional — two-legged signing leaves them empty and signs with just
    // `consumer_secret&` per RFC 5849 §3.4.2.
    const auto findVar = [&](std::string_view name) -> std::string {
        const auto it = session.variables.find(std::string{name});
        return (it == session.variables.end()) ? std::string{} : it->second;
    };
    const auto consumerKey    = findVar("consumer_key");
    const auto consumerSecret = findVar("consumer_secret");
    if (consumerKey.empty() || consumerSecret.empty()) return false;

    const auto token       = findVar("token");
    const auto tokenSecret = findVar("token_secret");
    const auto realm       = findVar("realm");

    const auto urlParts = splitUrl(req.url);
    if (!urlParts) return false;

    using Pair = std::pair<std::string, std::string>;
    std::vector<Pair> params;

    auto queryParams = parseQuery(urlParts->queryString);
    for (auto& p : queryParams) params.push_back(std::move(p));

    const auto contentTypeIt = req.headers.find("Content-Type");
    if (contentTypeIt != req.headers.end() &&
        contentTypeIt->second.starts_with("application/x-www-form-urlencoded") &&
        req.body) {
        auto bodyParams = parseQuery(*req.body);
        for (auto& p : bodyParams) params.push_back(std::move(p));
    }

    const std::string nonce =
        overrides.nonce.value_or(randomNonce());
    const std::string timestamp =
        overrides.timestampSeconds.value_or(nowUnixSeconds());

    params.emplace_back("oauth_consumer_key",     consumerKey);
    params.emplace_back("oauth_signature_method", "HMAC-SHA1");
    params.emplace_back("oauth_timestamp",        timestamp);
    params.emplace_back("oauth_nonce",            nonce);
    // `oauth_version` is OPTIONAL per RFC 5849 §3.1 with default "1.0".
    // Omitting it matches the RFC 5849 §3.4.3 worked example and maximises
    // interop with implementations that follow the RFC vector.
    if (!token.empty()) {
        params.emplace_back("oauth_token", token);
    }

    // Encode each name/value, then sort lexicographically by encoded name
    // then by encoded value (RFC 5849 §3.4.1.3.2).
    std::vector<Pair> encoded;
    encoded.reserve(params.size());
    for (const auto& [n, v] : params) {
        encoded.emplace_back(codecs::urlEncode(n), codecs::urlEncode(v));
    }
    std::sort(encoded.begin(), encoded.end());

    std::string paramString;
    for (const auto& [n, v] : encoded) {
        if (!paramString.empty()) paramString.push_back('&');
        paramString += n;
        paramString.push_back('=');
        paramString += v;
    }

    // Base string = METHOD & encode(URL) & encode(paramString).
    const std::string method = std::string{methodToString(req.method)};
    const std::string baseString =
        method + "&" + codecs::urlEncode(urlParts->base) +
        "&" + codecs::urlEncode(paramString);

    // Signing key = encode(consumer_secret) & encode(token_secret).
    // Trailing `&` is required even when token_secret is absent (RFC 5849 §3.4.2).
    const std::string signingKey =
        codecs::urlEncode(consumerSecret) + "&" +
        codecs::urlEncode(tokenSecret);

    const auto mac = crypto::hmacSha1(signingKey, baseString);
    if (mac.empty()) return false;
    const auto signature = codecs::base64Encode(mac);

    // Build Authorization header. Order matches the RFC 5849 §3.4.3 example;
    // `oauth_version` omitted.
    std::string authHeader = "OAuth ";
    if (!realm.empty()) {
        authHeader += "realm=\"" + codecs::urlEncode(realm) + "\", ";
    }
    const auto append = [&](std::string_view name, const std::string& value) {
        authHeader += name;
        authHeader += "=\"";
        authHeader += codecs::urlEncode(value);
        authHeader += "\", ";
    };
    append("oauth_consumer_key",     consumerKey);
    if (!token.empty()) append("oauth_token", token);
    append("oauth_signature_method", "HMAC-SHA1");
    append("oauth_timestamp",        timestamp);
    append("oauth_nonce",            nonce);
    authHeader += "oauth_signature=\"" + codecs::urlEncode(signature) + "\"";

    req.headers["Authorization"] = std::move(authHeader);
    return true;
}

}  // namespace chainapi::engine
