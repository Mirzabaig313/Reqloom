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

namespace reqloom::engine {

namespace {

using namespace codecs;

// 32 bytes from the OpenSSL CSPRNG, hex-encoded; empty when the generator fails.
std::string randomNonce() {
    return codecs::hexEncode(crypto::randomBytes(32));
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

std::string asciiLower(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

// RFC 5849 §3.4.1.2: scheme and host are lowercase and the default port (80 for http, 443 for
// https) is omitted; the path keeps its case.
std::string normalizedBaseUri(std::string_view base) {
    const auto schemeEnd = base.find("://");
    const auto scheme = asciiLower(base.substr(0, schemeEnd));
    const auto authorityStart = schemeEnd + 3;
    const auto pathStart = base.find('/', authorityStart);
    auto authority = base.substr(authorityStart, pathStart - authorityStart);
    const auto path =
        pathStart == std::string_view::npos ? std::string_view{"/"} : base.substr(pathStart);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    auto host = asciiLower(authority);
    const auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']', colon) == std::string::npos) {
        const auto port = std::string_view{host}.substr(colon + 1);
        if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443") ||
            port.empty()) {
            host.resize(colon);
        }
    }
    return scheme + "://" + host + std::string{path};
}

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
    std::string_view base = baseEnd == std::string_view::npos ? url : url.substr(0, baseEnd);
    if (pathStart == std::string_view::npos || pathStart > baseEnd) {
        // An empty path is "/" (RFC 5849 §3.4.1.2).
        out.base = normalizedBaseUri(std::string{base} + '/');
    } else {
        out.base = normalizedBaseUri(base);
    }

    if (queryStart != std::string_view::npos) {
        // qEnd is the fragment '#' position if it follows the query '?'; a '#' before '?' is
        // malformed, so take the query to end-of-URL.
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
        // urlDecode returns nullopt for malformed %-escapes; keep the encoded form.
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
    // Token + token_secret are optional: two-legged signing leaves them empty and signs with
    // `consumer_secret&` (RFC 5849 §3.4.2).
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

    const std::string nonce = overrides.nonce ? *overrides.nonce : randomNonce();
    if (nonce.empty()) {
        return false;
    }
    const std::string timestamp = overrides.timestampSeconds.value_or(nowUnixSeconds());

    params.emplace_back("oauth_consumer_key", consumerKey);
    params.emplace_back("oauth_signature_method", "HMAC-SHA1");
    params.emplace_back("oauth_timestamp", timestamp);
    params.emplace_back("oauth_nonce", nonce);
    // `oauth_version` is optional (RFC 5849 §3.1); omitting it matches the §3.4.3 worked example
    // and maximises interop.
    if (!token.empty()) {
        params.emplace_back("oauth_token", token);
    }

    // Encode, then sort by encoded name then value (RFC 5849 §3.4.1.3.2).
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

    const std::string method = std::string{methodToString(req.method)};
    const std::string baseString =
        method + "&" + codecs::urlEncode(urlParts->base) + "&" + codecs::urlEncode(paramString);

    // Signing key = encode(consumer_secret) & encode(token_secret); the trailing `&` is required
    // even with no token_secret (§3.4.2).
    const std::string signingKey =
        codecs::urlEncode(consumerSecret) + "&" + codecs::urlEncode(tokenSecret);

    const auto mac = crypto::hmacSha1(signingKey, baseString);
    if (mac.empty()) {
        return false;
    }
    const auto signature = codecs::base64Encode(mac);

    // Authorization header; order matches the RFC 5849 §3.4.3 example.
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

/// Lowercase ASCII in-place, byte by byte: locale-independent, unlike `std::tolower`.
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

/// Path component (incl. leading `/`) of a URL; `/` when there is none.
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

/// Percent-decode a URL path. Unlike form decoding, `+` stays a literal plus. nullopt on a
/// malformed escape.
std::optional<std::string> percentDecodePath(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    const auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '%') {
            out.push_back(input[i]);
            continue;
        }
        if (i + 2 >= input.size()) {
            return std::nullopt;
        }
        const int hi = hexValue(input[i + 1]);
        const int lo = hexValue(input[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

// AWS SigV4 "Create a canonical request": path segments are URI-encoded twice, except for Amazon
// S3, whose canonical URI is the path encoded once. The URL already holds the once-encoded path.
std::string canonicalUriFor(std::string_view path, std::string_view service) {
    if (service == "s3") {
        if (const auto decoded = percentDecodePath(path)) {
            return awsUriEncodePath(*decoded);
        }
        return std::string{path};
    }
    return awsUriEncodePath(path);
}

}  // namespace

std::optional<std::string> sigV4Refusal(const HttpRequest& req, const ActorSession& session) {
    const auto it = session.variables.find("service");
    const std::string service = it == session.variables.end() ? std::string{} : it->second;
    // The HTTP client serialises a multipart body (and picks its boundary) after signing, so
    // its hash is unknown here. Only S3 accepts UNSIGNED-PAYLOAD.
    if (req.multipart.empty() || service.empty() || service == "s3") {
        return std::nullopt;
    }
    return "AWS SigV4 cannot sign a multipart upload for service '" + service +
           "'; only s3 is supported";
}

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

    // Authorization is the signer's OUTPUT, never a signed header: drop one left by a prior attempt
    // so re-signing is idempotent.
    std::erase_if(stagedHeaders,
                  [](const auto& kv) { return toLowerAscii(kv.first) == "authorization"; });

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

    const std::string canonicalUri = canonicalUriFor(pathFromUrl(req.url), service);

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

    // SigV4 requires a signed Host; add it from the URL if missing.
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

    // Payload hash: sha256(body) hex. The header is required by S3 and harmless for other
    // services, so it is always emitted.
    std::string payloadHashHex;
    if (!req.multipart.empty()) {
        if (sigV4Refusal(req, session)) {
            return false;
        }
        payloadHashHex = "UNSIGNED-PAYLOAD";
    } else {
        const auto payloadHashRaw = crypto::sha256(req.body.value_or(std::string{}));
        if (payloadHashRaw.empty()) {
            return false;
        }
        payloadHashHex = codecs::hexEncode(payloadHashRaw);
    }
    stagedHeaders["x-amz-content-sha256"] = payloadHashHex;

    // SigV4 §3: signed header names are lowercase; headers differing only in case collapse into one
    // entry with values comma-joined (RFC 7230 §3.2.2).
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

    const std::string method = std::string{methodToString(req.method)};
    const std::string canonicalRequest = method + "\n" + canonicalUri + "\n" + canonicalQuery +
                                         "\n" + canonicalHeaders + "\n" + signedHeaders + "\n" +
                                         payloadHashHex;

    if (overrides.canonicalRequestOut != nullptr) {
        *overrides.canonicalRequestOut = canonicalRequest;
    }
    const auto canonicalRequestHash = crypto::sha256(canonicalRequest);
    if (canonicalRequestHash.empty()) {
        return false;
    }

    const std::string credentialScope = dateStamp + "/" + region + "/" + service + "/aws4_request";
    const std::string stringToSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + credentialScope +
                                     "\n" + codecs::hexEncode(canonicalRequestHash);

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

}  // namespace reqloom::engine
