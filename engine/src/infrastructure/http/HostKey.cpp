// Host keys for host approval — see reqloom/engine/HostApproval.h. Parsed with libcurl's URL API
// so the host compared is the host the request is sent to (`https://a.example@b.example/` is
// b.example), never a second parser's opinion of it.
#include <reqloom/engine/HostApproval.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

struct CurlUrlDeleter {
    void operator()(CURLU* p) const noexcept { curl_url_cleanup(p); }
};
struct CurlStringDeleter {
    void operator()(char* p) const noexcept { curl_free(p); }
};
using CurlUrl = std::unique_ptr<CURLU, CurlUrlDeleter>;
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

[[nodiscard]] ReqloomError keyError(std::string detail) {
    return ReqloomError{ErrorCode::HostNotApproved, ErrorClass::Auth, std::move(detail)};
}

[[nodiscard]] std::optional<std::string> urlPart(CURLU* url, CURLUPart part, unsigned flags = 0) {
    char* raw = nullptr;
    if (curl_url_get(url, part, &raw, flags) != CURLUE_OK || raw == nullptr) {
        return std::nullopt;
    }
    const CurlString owned{raw};
    return std::string{owned.get()};
}

[[nodiscard]] bool hasNonAscii(std::string_view text) noexcept {
    return std::ranges::any_of(text, [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

[[nodiscard]] std::string lowered(std::string_view s) {
    std::string out{s};
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

constexpr std::string_view kNonAsciiHint =
    "a host name with non-ASCII characters cannot be approved; write it in its ASCII (xn--) form";

/// `scheme://host:port` (or `host:port`) → (host, port); the host keeps IPv6 brackets.
[[nodiscard]] std::pair<std::string_view, std::string_view> splitKey(
    std::string_view key) noexcept {
    if (const auto sep = key.find("://"); sep != std::string_view::npos) {
        key.remove_prefix(sep + 3);
    }
    const auto colon = key.rfind(':');
    if (colon == std::string_view::npos ||
        (key.find(']') != std::string_view::npos && colon < key.find(']'))) {
        return {key, {}};
    }
    return {key.substr(0, colon), key.substr(colon + 1)};
}

/// True when an entry the person typed names a port: `h:1`, `[::1]:1`; not `h` or `[::1]`.
[[nodiscard]] bool entryHasPort(std::string_view entry) noexcept {
    if (entry.starts_with('[')) {
        const auto close = entry.find(']');
        return close != std::string_view::npos && close + 1 < entry.size() &&
               entry[close + 1] == ':';
    }
    return entry.find(':') != std::string_view::npos;
}

}  // namespace

std::expected<std::string, ReqloomError> hostKeyForUrl(std::string_view url) {
    if (url.empty()) {
        return std::unexpected(keyError("the request has no URL, so its host cannot be checked"));
    }
    const CurlUrl handle{curl_url()};
    if (!handle) {
        return std::unexpected(keyError("cannot parse the request URL (out of memory)"));
    }
    const std::string text{url};
    if (curl_url_set(handle.get(), CURLUPART_URL, text.c_str(), 0) != CURLUE_OK) {
        return std::unexpected(
            keyError(hasNonAscii(text) ? std::string{kNonAsciiHint}
                                       : "cannot read the host of the request URL; it must be an "
                                         "absolute http:// or https:// URL"));
    }
    const auto scheme = urlPart(handle.get(), CURLUPART_SCHEME);
    if (!scheme || (lowered(*scheme) != "http" && lowered(*scheme) != "https")) {
        return std::unexpected(keyError("only http:// and https:// URLs can carry credentials"));
    }
    auto host = urlPart(handle.get(), CURLUPART_HOST);
    const auto port = urlPart(handle.get(), CURLUPART_PORT, CURLU_DEFAULT_PORT);
    if (!host || host->empty() || !port) {
        return std::unexpected(keyError("the request URL has no host"));
    }
    if (hasNonAscii(*host) || host->find('%') != std::string::npos) {
        return std::unexpected(keyError(std::string{kNonAsciiHint}));
    }
    std::string key = lowered(*host);
    if (!key.starts_with('[')) {
        while (key.ends_with('.')) {
            key.pop_back();
        }
        if (key.empty()) {
            return std::unexpected(keyError("the request URL has no host"));
        }
    }
    return lowered(*scheme) + "://" + key + ":" + *port;
}

std::expected<std::string, ReqloomError> hostKeyForEntry(std::string_view entry) {
    auto text = trimmed(entry);
    const auto refuse = [&]() {
        return std::unexpected(keyError(
            "'" + std::string{entry} +
            "' is not a host: write a host name or IP address, optionally with http:// or "
            "https:// before it and :port after it (for example api.example.com, which means "
            "https on port 443, or http://127.0.0.1:3000)"));
    };
    std::string scheme{"https"};
    if (const auto sep = text.find("://"); sep != std::string_view::npos) {
        scheme = lowered(text.substr(0, sep));
        if (scheme != "http" && scheme != "https") {
            return refuse();
        }
        text.remove_prefix(sep + 3);
    }
    if (text.empty() || text.find_first_of("/@*?#%\\ \t,") != std::string_view::npos ||
        text.ends_with(':')) {
        return refuse();
    }
    if (hasNonAscii(text)) {
        return std::unexpected(keyError(std::string{kNonAsciiHint}));
    }
    auto key = hostKeyForUrl(scheme + "://" + std::string{text} + "/");
    if (!key) {
        return refuse();
    }
    if (const auto port = splitKey(*key).second; port.empty() || port == "0") {
        return refuse();
    }
    return key;
}

bool allowedHostMatches(std::string_view entry, std::string_view hostKey) {
    // `allowed_hosts` names hosts, not schemes: the scheme is part of the approval instead.
    auto text = trimmed(entry);
    if (text.find("://") != std::string_view::npos) {
        return false;
    }
    const auto key = hostKeyForEntry(text);
    if (!key) {
        return false;
    }
    if (entryHasPort(text)) {
        return splitKey(*key) == splitKey(hostKey);
    }
    return splitKey(*key).first == splitKey(hostKey).first;
}

std::expected<std::vector<std::string>, ReqloomError> parseHostList(std::string_view text) {
    std::vector<std::string> keys{};
    while (true) {
        const auto comma = text.find(',');
        const auto item = trimmed(text.substr(0, comma));
        if (!item.empty()) {
            auto key = hostKeyForEntry(item);
            if (!key) {
                return std::unexpected(key.error());
            }
            if (std::ranges::find(keys, *key) == keys.end()) {
                keys.push_back(std::move(*key));
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return keys;
}

}  // namespace reqloom::engine
