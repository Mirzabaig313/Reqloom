// RequestSigners — OAuth 1.0a signing per RFC 5849 §3.4.
#include "RequestSigners.h"

#include "../domain/Codecs.h"
#include "../infrastructure/util/Crypto.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine {

namespace {

using namespace codecs;

std::string randomNonce() {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::string nonce;
    nonce.reserve(64);
    for (int i = 0; i < 4; ++i) {
        const auto v = dist(gen);
        for (int j = 0; j < 8; ++j) {
            constexpr char kHex[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned>((v >> (j * 8)) & 0xFFU);
            nonce.push_back(kHex[byte >> 4]);
            nonce.push_back(kHex[byte & 0xF]);
        }
    }
    return nonce;
}

std::string nowUnixSeconds() {
    using namespace std::chrono;
    const auto secs = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    return std::to_string(secs);
}

struct UrlParts {
    std::string base;
    std::string queryString;
};

std::optional<UrlParts> splitUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) {
        return std::nullopt;
    }

    const auto pathStart = url.find('/', schemeEnd + 3);
    const auto queryStart = url.find('?', schemeEnd + 3);
    const auto fragStart = url.find('#', schemeEnd + 3);

    UrlParts out;
    const auto baseEnd = std::min(queryStart, fragStart);
    if (baseEnd == std::string_view::npos) {
        out.base = std::string{url};
    } else {
        out.base = std::string{url.substr(0, baseEnd)};
    }

    if (pathStart == std::string_view::npos ||
        (queryStart != std::string_view::npos && pathStart > queryStart) ||
        (fragStart != std::string_view::npos && pathStart > fragStart)) {
        // No path component — append "/" per RFC 5849 §3.4.1.2 which
        // requires the path with "/" as the default for empty.
        out.base += '/';
    }

    if (queryStart != std::string_view::npos) {
        // qEnd is the fragment '#' position, if it comes AFTER the query '?'.
        // If '#' comes before '?' the URL is malformed; take the query string
        // to end-of-URL as the least-surprising fallback.
        const auto qEnd = (fragStart == std::string_view::npos || fragStart < queryStart)
                              ? std::string_view::npos
                              : fragStart;
        out.queryString = std::string{url.substr(
            queryStart + 1,
            qEnd == std::string_view::npos ? std::string_view::npos : qEnd - queryStart - 1)};
    }
    return out;
}

/// Parse a query string into name/value pairs. Values are percent-decoded
/// so they can be re-encoded with the strict OAuth1 alphabet.
std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view qs) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < qs.size()) {
        const auto amp = qs.find('&', i);
        const auto piece =
            qs.substr(i, amp == std::string_view::npos ? std::string_view::npos : amp - i);
        const auto eq = piece.find('=');
        std::string name;
        std::string value;
        if (eq == std::string_view::npos) {
            name = std::string{piece};
        } else {
            name = std::string{piece.substr(0, eq)};
            value = std::string{piece.substr(eq + 1)};
        }
        // Decode then re-encode in the canonicalisation step.
        // urlDecode returns nullopt for malformed %-escapes; keep the
        // encoded form as-is.
        if (auto d = codecs::urlDecode(name); d) {
            name = std::move(*d);
        }
        if (auto d = codecs::urlDecode(value); d) {
            value = std::move(*d);
        }
        out.emplace_back(std::move(name), std::move(value));
        if (amp == std::string_view::npos) {
            break;
        }
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
    const auto consumerKey = findVar("consumer_key");
    const auto consumerSecret = findVar("consumer_secret");
    if (consumerKey.empty() || consumerSecret.empty()) {
        return false;
    }

    const auto token = findVar("token");
    const auto tokenSecret = findVar("token_secret");
    const auto realm = findVar("realm");

    const auto urlParts = splitUrl(req.url);
    if (!urlParts) {
        return false;
    }

    using Pair = std::pair<std::string, std::string>;
    std::vector<Pair> params;

    auto queryParams = parseQuery(urlParts->queryString);
    params.reserve(queryParams.size());
    for (auto& p : queryParams) {
        params.push_back(std::move(p));
    }

    const auto contentTypeIt = req.headers.find("Content-Type");
    if (contentTypeIt != req.headers.end() &&
        contentTypeIt->second.starts_with("application/x-www-form-urlencoded") && req.body) {
        auto bodyParams = parseQuery(*req.body);
        for (auto& p : bodyParams) {
            params.push_back(std::move(p));
        }
    }

    const std::string nonce = overrides.nonce.value_or(randomNonce());
    const std::string timestamp = overrides.timestampSeconds.value_or(nowUnixSeconds());

    params.emplace_back("oauth_consumer_key", consumerKey);
    params.emplace_back("oauth_signature_method", "HMAC-SHA1");
    params.emplace_back("oauth_timestamp", timestamp);
    params.emplace_back("oauth_nonce", nonce);
    // `oauth_version` is OPTIONAL per RFC 5849 §3.1 with default "1.0".
    // Omitting it matches the RFC 5849 §3.4.3 worked example and maximises
    // interop with implementations that follow the RFC vector.
    if (!token.empty()) {
        params.emplace_back("oauth_token", token);
    }

    // Encode each name/value, then sort lexicographically by encoded name
    // then by encoded value (RFC 5849 ).
    std::vector<Pair> encoded;
    encoded.reserve(params.size());
    for (const auto& [n, v] : params) {
        encoded.emplace_back(codecs::urlEncode(n), codecs::urlEncode(v));
    }
    std::sort(encoded.begin(), encoded.end());

    std::string paramString;
    for (const auto& [n, v] : encoded) {
        if (!paramString.empty()) {
            paramString.push_back('&');
        }
        paramString += n;
        paramString.push_back('=');
        paramString += v;
    }

    // Base string = METHOD & encode(URL) & encode(paramString).
    const std::string method = std::string{methodToString(req.method)};
    const std::string baseString =
        method + "&" + codecs::urlEncode(urlParts->base) + "&" + codecs::urlEncode(paramString);

    // Signing key = encode(consumer_secret) & encode(token_secret).
    // Trailing `&` is required even when token_secret is absent (RFC 5849 §3.4.2).
    const std::string signingKey =
        codecs::urlEncode(consumerSecret) + "&" + codecs::urlEncode(tokenSecret);

    const auto mac = crypto::hmacSha1(signingKey, baseString);
    if (mac.empty()) {
        return false;
    }
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
    append("oauth_consumer_key", consumerKey);
    if (!token.empty()) {
        append("oauth_token", token);
    }
    append("oauth_signature_method", "HMAC-SHA1");
    append("oauth_timestamp", timestamp);
    append("oauth_nonce", nonce);
    authHeader += "oauth_signature=\"" + codecs::urlEncode(signature) + "\"";

    req.headers["Authorization"] = std::move(authHeader);
    return true;
}

// AWS Signature Version 4

namespace {

/// AWS-flavoured URI encoding for path segments.
/// Does NOT encode `/`, `~`, `-`, `.`, `_`. Encodes `+`, `=`, `&`, etc.
std::string awsUriEncodePath(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto byte : input) {
        const auto u = static_cast<unsigned char>(byte);
        const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                                (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_' ||
                                u == '~' || u == '/';
        if (unreserved) {
            out.push_back(byte);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0xF]);
        }
    }
    return out;
}

/// AWS-flavoured URI encoding for query keys and values.
/// Stricter than `awsUriEncodePath`: `/` is also encoded.
std::string awsUriEncodeQuery(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const auto byte : input) {
        const auto u = static_cast<unsigned char>(byte);
        const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                                (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_' ||
                                u == '~';
        if (unreserved) {
            out.push_back(byte);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0xF]);
        }
    }
    return out;
}

/// Lowercase ASCII in-place. AWS canonicalisation uses lowercased
/// header names; locale-independent behaviour is required so we do
/// it byte-by-byte rather than via `std::tolower`.
std::string toLowerAscii(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

/// Trim leading/trailing ASCII whitespace and collapse interior runs of
/// whitespace into single spaces — AWS spec for canonical header values.
std::string trimAndCollapseWs(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool inWs = true;  // skip leading whitespace
    for (const auto c : s) {
        if (c == ' ' || c == '\t') {
            if (!inWs) {
                out.push_back(' ');
                inWs = true;
            }
        } else {
            out.push_back(c);
            inWs = false;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// Extract host[:port] from a URL. Empty string when unparseable.
std::string hostFromUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) {
        return {};
    }
    const auto hostStart = schemeEnd + 3;
    auto hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string_view::npos) {
        hostEnd = url.size();
    }
    return std::string{url.substr(hostStart, hostEnd - hostStart)};
}

/// Extract the path component (incl. leading `/`) from a URL. Returns
/// `/` when the URL has no path.
std::string pathFromUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) {
        return "/";
    }
    const auto pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string_view::npos) {
        return "/";
    }
    auto pathEnd = url.find_first_of("?#", pathStart);
    if (pathEnd == std::string_view::npos) {
        pathEnd = url.size();
    }
    return std::string{url.substr(pathStart, pathEnd - pathStart)};
}

}  // namespace

bool signSigV4Request(HttpRequest& req,
                      const ActorSession& session,
                      const SigV4TestOverrides& overrides) {
    const auto findVar = [&](std::string_view name) -> std::string {
        const auto it = session.variables.find(std::string{name});
        return (it == session.variables.end()) ? std::string{} : it->second;
    };
    const auto accessKey = findVar("access_key");
    const auto secretKey = findVar("secret_key");
    const auto region = findVar("region");
    const auto service = findVar("service");
    const auto sessionToken = findVar("session_token");
    if (accessKey.empty() || secretKey.empty() || region.empty() || service.empty()) {
        return false;
    }

    const auto urlParts = splitUrl(req.url);
    if (!urlParts) {
        return false;
    }

    // Stage header mutations on a local copy; commit only on success so a
    // partial failure leaves the request untouched.
    std::map<std::string, std::string> stagedHeaders = req.headers;

    // 1. Timestamp + date scope
    std::string amzDate;
    if (overrides.amzDate) {
        amzDate = *overrides.amzDate;
    } else {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        amzDate = std::format("{:04}{:02}{:02}T{:02}{:02}{:02}Z",
                              tm.tm_year + 1900,
                              tm.tm_mon + 1,
                              tm.tm_mday,
                              tm.tm_hour,
                              tm.tm_min,
                              tm.tm_sec);
    }
    if (amzDate.size() < 8) {
        return false;
    }
    const std::string dateStamp = amzDate.substr(0, 8);

    // 2. Canonical URI + query string
    const std::string canonicalUri = awsUriEncodePath(pathFromUrl(req.url));

    using Pair = std::pair<std::string, std::string>;
    auto rawQuery = parseQuery(urlParts->queryString);
    std::vector<Pair> encQuery;
    encQuery.reserve(rawQuery.size());
    for (const auto& [n, v] : rawQuery) {
        encQuery.emplace_back(awsUriEncodeQuery(n), awsUriEncodeQuery(v));
    }
    std::sort(encQuery.begin(), encQuery.end());
    std::string canonicalQuery;
    for (const auto& [n, v] : encQuery) {
        if (!canonicalQuery.empty()) {
            canonicalQuery.push_back('&');
        }
        canonicalQuery += n;
        canonicalQuery.push_back('=');
        canonicalQuery += v;
    }

    // 3. Headers we sign — SigV4 requires Host.
    // Add it from the URL if the caller didn't.
    bool haveHost = false;
    for (const auto& [name, _] : stagedHeaders) {
        if (toLowerAscii(name) == "host") {
            haveHost = true;
            break;
        }
    }
    if (!haveHost) {
        const auto host = hostFromUrl(req.url);
        if (host.empty()) {
            return false;
        }
        stagedHeaders["Host"] = host;
    }
    stagedHeaders["x-amz-date"] = amzDate;
    if (!sessionToken.empty()) {
        stagedHeaders["x-amz-security-token"] = sessionToken;
    }

    // Payload hash: sha256(body) hex. Empty body has the well-known
    // empty-string hash. The header is required by S3 and harmless for
    // other services, so we always emit it.
    const std::string body = req.body.value_or(std::string{});
    const auto payloadHashRaw = crypto::sha256(body);
    if (payloadHashRaw.empty()) {
        return false;
    }
    const std::string payloadHashHex = codecs::hexEncode(payloadHashRaw);
    stagedHeaders["x-amz-content-sha256"] = payloadHashHex;

    // Build the sorted lowercased header list.
    //
    // SigV4 §3 requires every signed header name to be lowercase. Two
    // headers with the same case-insensitive name (`Host` and `host`)
    // collapse into one canonical entry; their values are merged into a
    // comma-separated list per RFC 7230 §3.2.2 multi-value header rules.
    std::map<std::string, std::string> mergedHeaders;
    for (const auto& [name, value] : stagedHeaders) {
        const auto key = toLowerAscii(name);
        const auto trimmed = trimAndCollapseWs(value);
        if (auto it = mergedHeaders.find(key); it != mergedHeaders.end()) {
            it->second += ',';
            it->second += trimmed;
        } else {
            mergedHeaders.emplace(key, trimmed);
        }
    }

    std::string canonicalHeaders;
    std::string signedHeaders;
    for (const auto& [n, v] : mergedHeaders) {
        canonicalHeaders += n;
        canonicalHeaders.push_back(':');
        canonicalHeaders += v;
        canonicalHeaders.push_back('\n');
        if (!signedHeaders.empty()) {
            signedHeaders.push_back(';');
        }
        signedHeaders += n;
    }

    // 4. Canonical request → string-to-sign
    const std::string method = std::string{methodToString(req.method)};
    const std::string canonicalRequest = method + "\n" + canonicalUri + "\n" + canonicalQuery +
                                         "\n" + canonicalHeaders + "\n" + signedHeaders + "\n" +
                                         payloadHashHex;

    const auto canonicalRequestHash = crypto::sha256(canonicalRequest);
    if (canonicalRequestHash.empty()) {
        return false;
    }

    const std::string credentialScope = dateStamp + "/" + region + "/" + service + "/aws4_request";
    const std::string stringToSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + credentialScope +
                                     "\n" + codecs::hexEncode(canonicalRequestHash);

    // 5. Signing key derivation (4× HMAC-SHA256 chain)
    const auto kDate = crypto::hmacSha256("AWS4" + secretKey, dateStamp);
    const auto kRegion = crypto::hmacSha256(kDate, region);
    const auto kService = crypto::hmacSha256(kRegion, service);
    const auto kSigning = crypto::hmacSha256(kService, "aws4_request");
    if (kDate.empty() || kRegion.empty() || kService.empty() || kSigning.empty()) {
        return false;
    }

    const auto signatureRaw = crypto::hmacSha256(kSigning, stringToSign);
    if (signatureRaw.empty()) {
        return false;
    }
    const std::string signatureHex = codecs::hexEncode(signatureRaw);

    // 6. Authorization header
    std::string authHeader = "AWS4-HMAC-SHA256 Credential=";
    authHeader += accessKey;
    authHeader += '/';
    authHeader += credentialScope;
    authHeader += ", SignedHeaders=";
    authHeader += signedHeaders;
    authHeader += ", Signature=";
    authHeader += signatureHex;
    stagedHeaders["Authorization"] = std::move(authHeader);

    req.headers = std::move(stagedHeaders);
    return true;
}

}  // namespace chainapi::engine
