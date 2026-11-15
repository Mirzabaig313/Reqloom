// Cookies — see header. RFC 6265 §5.1 (dates, domain and path matching), §5.2 (parsing).

#include "Cookies.h"

#include <reqloom/engine/RunContext.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>

namespace reqloom::engine::cookies {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char const c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// RFC 6265 §5.1.1 delimiter set.
[[nodiscard]] bool isDateDelimiter(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u == 0x09 || (u >= 0x20 && u <= 0x2F) || (u >= 0x3B && u <= 0x40) ||
           (u >= 0x5B && u <= 0x60) || (u >= 0x7B && u <= 0x7E);
}

// Leading run of 1..maxDigits digits not followed by another digit; nullopt otherwise.
[[nodiscard]] std::optional<int> leadingNumber(std::string_view token,
                                               std::size_t minDigits,
                                               std::size_t maxDigits) {
    std::size_t n = 0;
    while (n < token.size() && isDigit(token[n])) {
        ++n;
    }
    if (n < minDigits || n > maxDigits) {
        return std::nullopt;
    }
    int value = 0;
    std::from_chars(token.data(), token.data() + n, value);
    return value;
}

// hms-time = 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT, optionally followed by non-digits.
[[nodiscard]] bool parseTime(std::string_view token, int& h, int& m, int& s) {
    std::array<int, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto value = leadingNumber(token, 1, 2);
        if (!value) {
            return false;
        }
        parts.at(i) = *value;
        const auto digits = token.find_first_not_of("0123456789");
        token.remove_prefix(digits == std::string_view::npos ? token.size() : digits);
        if (i < 2) {
            if (!token.starts_with(':')) {
                return false;
            }
            token.remove_prefix(1);
        }
    }
    h = parts[0];
    m = parts[1];
    s = parts[2];
    return true;
}

// RFC 6265 §5.1.1 cookie-date algorithm.
[[nodiscard]] std::optional<std::chrono::sys_seconds> parseCookieDate(std::string_view text) {
    static constexpr std::array<std::string_view, 12> kMonths = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::optional<int> hour;
    int minute = 0;
    int second = 0;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isDateDelimiter(text[i])) {
            ++i;
        }
        const auto start = i;
        while (i < text.size() && !isDateDelimiter(text[i])) {
            ++i;
        }
        const auto token = text.substr(start, i - start);
        if (token.empty()) {
            continue;
        }
        int h = 0;
        int m = 0;
        int s = 0;
        if (!hour && parseTime(token, h, m, s)) {
            hour = h;
            minute = m;
            second = s;
        } else if (const auto d = leadingNumber(token, 1, 2); !day && d) {
            day = d;
        } else if (!month && token.size() >= 3) {
            const auto prefix = toLowerCopy(token.substr(0, 3));
            for (std::size_t k = 0; k < kMonths.size(); ++k) {
                if (kMonths.at(k) == prefix) {
                    month = static_cast<int>(k) + 1;
                }
            }
            if (!month) {
                if (const auto y = leadingNumber(token, 2, 4); !year && y) {
                    year = y;
                }
            }
        } else if (const auto y = leadingNumber(token, 2, 4); !year && y) {
            year = y;
        }
    }
    if (!hour || !day || !month || !year) {
        return std::nullopt;
    }
    int y = *year;
    if (y >= 70 && y <= 99) {
        y += 1900;
    } else if (y >= 0 && y <= 69) {
        y += 2000;
    }
    if (*day < 1 || *day > 31 || y < 1601 || *hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(*month)},
                                          std::chrono::day{static_cast<unsigned>(*day)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{ymd} + std::chrono::hours{*hour} + std::chrono::minutes{minute} +
           std::chrono::seconds{second};
}

[[nodiscard]] bool isIpAddress(std::string_view host) {
    if (host.find(':') != std::string_view::npos || host.starts_with('[')) {
        return true;
    }
    return !host.empty() && host.find_first_not_of("0123456789.") == std::string_view::npos;
}

[[nodiscard]] std::string_view pathOf(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) {
        return "/";
    }
    const auto pathStart = url.find_first_of("/?#", schemeEnd + 3);
    if (pathStart == std::string_view::npos || url[pathStart] != '/') {
        return "/";
    }
    const auto pathEnd = url.find_first_of("?#", pathStart);
    return url.substr(
        pathStart,
        pathEnd == std::string_view::npos ? std::string_view::npos : pathEnd - pathStart);
}

// RFC 6265 §5.1.4 path-match.
[[nodiscard]] bool pathMatches(std::string_view requestPath, std::string_view cookiePath) {
    if (requestPath == cookiePath) {
        return true;
    }
    if (!requestPath.starts_with(cookiePath)) {
        return false;
    }
    return cookiePath.ends_with('/') || requestPath[cookiePath.size()] == '/';
}

}  // namespace

bool SetCookie::removes(std::chrono::system_clock::time_point now) const {
    if (value.empty()) {
        return true;
    }
    if (maxAge) {
        return *maxAge <= 0;
    }
    return expires.has_value() && *expires < now;
}

std::optional<std::pair<std::string, std::string>> parseSetCookie(std::string_view header) {
    // The cookie is everything up to the first `;`; the rest are attributes.
    auto semi = header.find(';');
    auto cookie = (semi == std::string_view::npos) ? header : header.substr(0, semi);

    auto eq = cookie.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }

    auto name = trim(cookie.substr(0, eq));
    auto value = trim(cookie.substr(eq + 1));
    if (name.empty()) {
        return std::nullopt;
    }

    return std::make_pair(std::string{name}, std::string{value});
}

std::optional<SetCookie> parseSetCookieHeader(std::string_view header) {
    auto pair = parseSetCookie(header);
    if (!pair) {
        return std::nullopt;
    }
    SetCookie out;
    out.name = std::move(pair->first);
    out.value = std::move(pair->second);

    auto rest = header.substr(std::min(header.find(';'), header.size()));
    while (!rest.empty()) {
        rest.remove_prefix(1);
        const auto next = rest.find(';');
        const auto attr = rest.substr(0, next);
        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);

        const auto eq = attr.find('=');
        const auto key = toLowerCopy(trim(attr.substr(0, eq)));
        const auto value =
            eq == std::string_view::npos ? std::string_view{} : trim(attr.substr(eq + 1));
        if (key == "secure") {
            out.secure = true;
        } else if (key == "domain" && !value.empty()) {
            auto domain = toLowerCopy(value);
            if (domain.starts_with('.')) {
                domain.erase(0, 1);
            }
            if (!domain.empty()) {
                out.domain = std::move(domain);
            }
        } else if (key == "path" && value.starts_with('/')) {
            out.path = std::string{value};
        } else if (key == "max-age" && !value.empty()) {
            std::int64_t parsed = 0;
            const auto* end = value.data() + value.size();
            const auto [ptr, ec] = std::from_chars(value.data(), end, parsed);
            if (ptr == end && ec == std::errc{}) {
                out.maxAge = parsed;
            } else if (ptr == end && ec == std::errc::result_out_of_range) {
                out.maxAge = value.starts_with('-') ? -1 : INT64_MAX;
            }
        } else if (key == "expires") {
            if (auto date = parseCookieDate(value)) {
                out.expires = *date;
            }
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> collectFromResponse(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        if (toLowerCopy(k) != "set-cookie") {
            continue;
        }
        if (auto parsed = parseSetCookie(v); parsed) {
            out.push_back(std::move(*parsed));
        }
    }
    return out;
}

void absorbSetCookies(const std::vector<std::pair<std::string, std::string>>& headers,
                      const ActorId& actor,
                      RunContext& ctx,
                      std::string_view requestUrl) {
    const auto now = std::chrono::system_clock::now();
    const auto host = hostOf(requestUrl);
    for (const auto& [k, v] : headers) {
        if (toLowerCopy(k) != "set-cookie") {
            continue;
        }
        auto cookie = parseSetCookieHeader(v);
        if (!cookie) {
            continue;
        }
        if (cookie->removes(now)) {
            ctx.removeCookie(actor, cookie->name, host);
            continue;
        }
        JarCookie stored{.name = std::move(cookie->name),
                         .value = std::move(cookie->value),
                         .setterHost = host,
                         .domain = std::move(cookie->domain),
                         .path = std::move(cookie->path),
                         .secure = cookie->secure};
        if (cookie->maxAge) {
            stored.expiresAt = now + std::chrono::seconds{std::min<std::int64_t>(
                                         *cookie->maxAge, std::int64_t{10} * 365 * 24 * 3600)};
        } else if (cookie->expires) {
            stored.expiresAt = *cookie->expires;
        }
        ctx.storeCookie(actor, std::move(stored));
    }
}

std::string hostOf(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) {
        return {};
    }
    auto authority = url.substr(schemeEnd + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    if (authority.starts_with('[')) {
        return toLowerCopy(authority.substr(0, authority.find(']') + 1));
    }
    return toLowerCopy(authority.substr(0, authority.find(':')));
}

bool domainMatches(std::string_view host, std::string_view domain) {
    const auto h = toLowerCopy(host);
    const auto d = toLowerCopy(domain);
    if (h == d) {
        return true;
    }
    return !d.empty() && h.size() > d.size() && h.ends_with(d) &&
           h[h.size() - d.size() - 1] == '.' && !isIpAddress(h);
}

bool shouldSend(const SetCookie& cookie, std::string_view setterHost, std::string_view requestUrl) {
    const auto scheme = toLowerCopy(requestUrl.substr(0, requestUrl.find("://")));
    if (cookie.secure && scheme != "https") {
        return false;
    }
    const auto requestHost = hostOf(requestUrl);
    if (cookie.domain) {
        // A response may only set a cookie for its own host or a parent domain of it.
        if (!domainMatches(setterHost, *cookie.domain) ||
            !domainMatches(requestHost, *cookie.domain)) {
            return false;
        }
    } else if (requestHost != toLowerCopy(setterHost)) {
        return false;
    }
    return !cookie.path || pathMatches(pathOf(requestUrl), *cookie.path);
}

bool shouldSend(const JarCookie& cookie,
                std::string_view requestUrl,
                const std::chrono::system_clock::time_point now) {
    if (cookie.expiresAt && *cookie.expiresAt <= now) {
        return false;
    }
    const SetCookie scope{.name = cookie.name,
                          .value = cookie.value,
                          .domain = cookie.domain,
                          .path = cookie.path,
                          .secure = cookie.secure};
    const std::string setter = cookie.setterHost.empty() ? hostOf(requestUrl) : cookie.setterHost;
    return shouldSend(scope, setter, requestUrl);
}

std::string requestHeaderFor(const std::vector<JarCookie>& jar,
                             std::string_view requestUrl,
                             const std::chrono::system_clock::time_point now) {
    std::map<std::string, std::string> sendable;
    for (const auto& cookie : jar) {
        if (shouldSend(cookie, requestUrl, now)) {
            sendable[cookie.name] = cookie.value;
        }
    }
    return formatRequestHeader(sendable);
}

std::string formatRequestHeader(const std::map<std::string, std::string>& jar) {
    std::string out;
    for (const auto& [name, value] : jar) {
        if (!out.empty()) {
            out += "; ";
        }
        out += name;
        out += "=";
        out += value;
    }
    return out;
}

}  // namespace reqloom::engine::cookies
