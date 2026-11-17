// SecretRedaction — see header.

#include "SecretRedaction.h"

#include "../domain/Codecs.h"
#include "../domain/SecretRegistry.h"
#include "HeaderMasking.h"
#include "JsonExtraction.h"
#include "PredicateEvaluator.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace reqloom::engine {

namespace {

using ordered_json = nlohmann::ordered_json;

/// Sensitive-name policy for body fields: the header policy applied to the field name, widened
/// for agent-facing output (`jwt`, `session_id`).
[[nodiscard]] bool isSensitiveField(std::string_view name, const MaskProfile profile) noexcept {
    return profile == MaskProfile::Agent ? isAgentSensitiveName(name) : isSensitiveHeader(name);
}

/// Text that can be quoted in a detail: valid UTF-8 with no control bytes
/// other than tab / CR / LF.
[[nodiscard]] bool isSafeText(std::string_view s) noexcept {
    std::size_t i{};
    while (i < s.size()) {
        const auto byte = static_cast<unsigned char>(s[i]);
        if (byte < 0x80U) {
            if ((byte < 0x20U && byte != '\t' && byte != '\n' && byte != '\r') || byte == 0x7FU) {
                return false;
            }
            ++i;
            continue;
        }
        std::size_t extra{};
        if ((byte & 0xE0U) == 0xC0U && byte >= 0xC2U) {
            extra = 1;
        } else if ((byte & 0xF0U) == 0xE0U) {
            extra = 2;
        } else if ((byte & 0xF8U) == 0xF0U && byte <= 0xF4U) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

[[nodiscard]] bool isKeyChar(const char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return std::isalnum(byte) != 0 || c == '_' || c == '-' || c == '.';
}

[[nodiscard]] bool isInlineSpace(const char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool endsBareValue(const char c) noexcept {
    return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '&' || c == ';' || c == ',' ||
           c == '"' || c == '\'' || c == '<' || c == '>' || c == '}' || c == ']';
}

/// `<name>value</...`: when `name` is sensitive, append the open tag and the
/// marker and return the index of the next '<'; else nullopt.
[[nodiscard]] std::optional<std::size_t> redactXmlElement(std::string_view s,
                                                          const std::size_t open,
                                                          const MaskProfile profile,
                                                          std::string& out) {
    std::size_t j = open + 1;
    while (j < s.size() && (isKeyChar(s[j]) || s[j] == ':')) {
        ++j;
    }
    if (j == open + 1 || j >= s.size() || s[j] != '>' ||
        !isSensitiveField(s.substr(open + 1, j - open - 1), profile)) {
        return std::nullopt;
    }
    const auto close = s.find('<', j + 1);
    const auto end = close == std::string_view::npos ? s.size() : close;
    out.append(s.substr(open, j + 1 - open));
    if (end > j + 1) {
        out.append(kRedactedValueMarker);
    }
    return end;
}

/// Index just past the value starting at `k`, appending the masked value.
[[nodiscard]] std::size_t redactValueAt(std::string_view s,
                                        std::size_t k,
                                        const char separator,
                                        std::string& out) {
    if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
        const char quote = s[k];
        std::size_t close = k + 1;
        while (close < s.size() && s[close] != quote) {
            close += s[close] == '\\' ? 2 : 1;
        }
        out.push_back(quote);
        out.append(kRedactedValueMarker);
        if (close < s.size()) {
            out.push_back(quote);
            return close + 1;
        }
        return s.size();
    }
    std::size_t end = k;
    if (separator == ':') {
        // Header / YAML style: the value runs to the end of the line, so a
        // scheme word ("Bearer abc") cannot leave the credential behind.
        end = s.find_first_of("\r\n", k);
        end = end == std::string_view::npos ? s.size() : end;
    } else {
        while (end < s.size() && !endsBareValue(s[end])) {
            ++end;
        }
    }
    if (end > k) {
        out.append(kRedactedValueMarker);
    }
    return end;
}

/// Conservative scan of free text for `key=value`, `key: value` and
/// `<key>value</key>` with a sensitive key; such values become the marker.
[[nodiscard]] std::string redactKeyValueText(std::string_view s,
                                             const MaskProfile profile = MaskProfile::Display) {
    std::string out{};
    out.reserve(s.size());
    std::size_t i{};
    while (i < s.size()) {
        const char c = s[i];
        if (c == '<') {
            if (const auto next = redactXmlElement(s, i, profile, out)) {
                i = *next;
                continue;
            }
        }
        if (!isKeyChar(c) || (i > 0 && isKeyChar(s[i - 1]))) {
            out.push_back(c);
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < s.size() && isKeyChar(s[j])) {
            ++j;
        }
        const auto key = s.substr(i, j - i);
        std::size_t k = j;
        // A quoted key ("access_token": …) keeps its closing quote.
        if (i > 0 && k < s.size() && (s[i - 1] == '"' || s[i - 1] == '\'') && s[k] == s[i - 1]) {
            ++k;
        }
        while (k < s.size() && isInlineSpace(s[k])) {
            ++k;
        }
        if (k >= s.size() || (s[k] != '=' && s[k] != ':') || !isSensitiveField(key, profile)) {
            out.append(key);
            i = j;
            continue;
        }
        const char separator = s[k];
        ++k;
        while (k < s.size() && isInlineSpace(s[k])) {
            ++k;
        }
        out.append(s.substr(i, k - i));
        i = redactValueAt(s, k, separator, out);
    }
    return out;
}

/// Masks values by field name. With a registry (agent profile only), registered secrets are also
/// masked while decoded: in string values, in a key only when the key itself holds a secret, and
/// in a number whose text holds one (a PIN or account number returned as a JSON number).
void redactJsonNode(ordered_json& node, const MaskProfile profile, const SecretRegistry* registry) {
    if (node.is_object()) {
        bool keysHoldSecrets = false;
        for (auto it = node.begin(); registry != nullptr && it != node.end(); ++it) {
            keysHoldSecrets = keysHoldSecrets || registry->containsAny(it.key());
        }
        if (keysHoldSecrets) {
            ordered_json renamed = ordered_json::object();
            for (auto it = node.begin(); it != node.end(); ++it) {
                renamed[registry->redact(it.key())] = std::move(it.value());
            }
            node = std::move(renamed);
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (isSensitiveField(it.key(), profile)) {
                it.value() = std::string{kRedactedValueMarker};
            } else {
                redactJsonNode(it.value(), profile, registry);
            }
        }
    } else if (node.is_array()) {
        for (auto& element : node) {
            redactJsonNode(element, profile, registry);
        }
    } else if (node.is_string()) {
        // A message string can itself carry `token=…` text.
        auto text = redactKeyValueText(node.get_ref<const std::string&>(), profile);
        node = registry != nullptr ? registry->redact(text) : std::move(text);
    } else if (node.is_number() && registry != nullptr && registry->containsAny(node.dump())) {
        node = std::string{kRedactedValueMarker};
    }
}

/// Agent profile, before parsing: a number literal (outside strings) whose text holds a
/// registered secret becomes the marker string. Checked on the text as received, because a number
/// too long for 64 bits is parsed as a double and printed back in exponent form.
[[nodiscard]] std::string maskSecretNumberLiterals(std::string_view text,
                                                   const SecretRegistry& registry) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (inString) {
            out.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                out.push_back(text[i + 1]);
                i += 2;
                continue;
            }
            inString = c != '"';
            ++i;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t end = i + 1;
            while (end < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[end])) != 0 || text[end] == '.' ||
                    text[end] == 'e' || text[end] == 'E' || text[end] == '+' || text[end] == '-')) {
                ++end;
            }
            const auto literal = text.substr(i, end - i);
            if (registry.containsAny(literal)) {
                out += '"';
                out += kRedactedValueMarker;
                out += '"';
            } else {
                out += literal;
            }
            i = end;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/// Append code point `cp` to `out` as UTF-8.
void appendUtf8(std::string& out, const std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

[[nodiscard]] std::optional<std::uint32_t> hex4(std::string_view s, const std::size_t at) {
    if (at + 4 > s.size()) {
        return std::nullopt;
    }
    std::uint32_t value{};
    for (std::size_t i = at; i < at + 4; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        value <<= 4U;
        if (std::isdigit(c) != 0) {
            value |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            value |= c - 'a' + 10U;
        } else if (c >= 'A' && c <= 'F') {
            value |= c - 'A' + 10U;
        } else {
            return std::nullopt;
        }
    }
    return value;
}

/// The text a JSON reader gets from the inside of a string literal: every escape decoded (`\/`,
/// `\uXXXX` with surrogate pairs, `\n`, …). A literal cut short ends at the cut.
[[nodiscard]] std::string decodeJsonLiteral(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') {
            out.push_back(raw[i]);
            continue;
        }
        if (++i >= raw.size()) {
            break;
        }
        switch (raw[i]) {
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                auto cp = hex4(raw, i + 1);
                if (!cp) {
                    return out;
                }
                i += 4;
                if (*cp >= 0xD800 && *cp < 0xDC00 && i + 2 < raw.size() && raw[i + 1] == '\\' &&
                    raw[i + 2] == 'u') {
                    if (const auto low = hex4(raw, i + 3); low && *low >= 0xDC00 && *low < 0xE000) {
                        cp = 0x10000 + ((*cp - 0xD800) << 10U) + (*low - 0xDC00);
                        i += 6;
                    }
                }
                appendUtf8(out, *cp);
                break;
            }
            default:
                out.push_back(raw[i]);
                break;  // `"`, `\\`, `/`
        }
    }
    return out;
}

/// Agent profile, for text that looks like JSON but does not parse (a body cut at the retention
/// limit): any spelling of a secret can sit inside a string literal (`\/`, `\uXXXX`), so each
/// literal is decoded as a JSON reader would and replaced whole by the marker when the decoded
/// text holds a registered secret; text outside literals is value-redacted as written.
[[nodiscard]] std::string redactJsonLiterals(std::string_view text,
                                             const SecretRegistry& registry) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto open = text.find('"', pos);
        out += registry.redact(
            text.substr(pos, open == std::string_view::npos ? text.npos : open - pos));
        if (open == std::string_view::npos) {
            break;
        }
        std::size_t close = open + 1;
        while (close < text.size() && text[close] != '"') {
            close += text[close] == '\\' ? 2 : 1;
        }
        const bool terminated = close < text.size();
        const auto raw = text.substr(open + 1, std::min(close, text.size()) - open - 1);
        const auto decoded = decodeJsonLiteral(raw);
        out.push_back('"');
        if (registry.containsAny(decoded) || registry.containsAny(raw)) {
            out += kRedactedValueMarker;
        } else {
            out += raw;
        }
        if (terminated) {
            out.push_back('"');
        }
        pos = terminated ? close + 1 : text.size();
    }
    return out;
}

[[nodiscard]] bool looksLikeJson(std::string_view body) noexcept {
    const auto first = body.find_first_not_of(" \t\r\n");
    return first != std::string_view::npos && (body[first] == '{' || body[first] == '[');
}

void redactString(std::string& s, const SecretRegistry& registry) {
    if (!s.empty()) {
        s = registry.redact(s);
    }
}

void redactHeaderValues(std::vector<std::pair<std::string, std::string>>& headers,
                        const SecretRegistry& registry) {
    for (auto& [name, value] : headers) {
        redactString(value, registry);
    }
}

/// "812 bytes", "5.9 KiB", "2.3 MiB".
[[nodiscard]] std::string formatByteSize(const std::size_t bytes) {
    constexpr double kKiB = 1024.0;
    if (bytes < 1024) {
        return std::to_string(bytes) + " bytes";
    }
    if (bytes < 1024 * 1024) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / kKiB);
    }
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (kKiB * kKiB));
}

/// `text` cut to `limit` bytes on a UTF-8 boundary plus a marker naming the response's
/// `totalBytes`, or `text` unchanged when it fits.
[[nodiscard]] std::string capExcerpt(std::string text,
                                     const std::size_t limit,
                                     const std::size_t totalBytes) {
    if (text.size() <= limit) {
        return text;
    }
    return codecs::truncateUtf8(text, limit) + "… (truncated, " + formatByteSize(totalBytes) +
           " total)";
}

/// Field name a single-value path ends with (`$.data.access_token` → `access_token`); empty
/// when it ends with an index or filter.
[[nodiscard]] std::string_view lastFieldName(std::string_view path) {
    if (path.ends_with(']')) {
        return {};
    }
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? path : path.substr(dot + 1);
}

/// One selected value as failure text, masked like the default excerpt.
[[nodiscard]] std::string maskedSelection(std::string_view path,
                                          const nlohmann::json& value,
                                          const MaskProfile profile,
                                          const SecretRegistry* registry) {
    // Naming a credential field in `show` does not unmask it.
    if (isSensitiveField(lastFieldName(path), profile)) {
        return std::string{kRedactedValueMarker};
    }
    const SecretRegistry* leaves = profile == MaskProfile::Agent ? registry : nullptr;
    if (value.is_string()) {
        auto text = redactKeyValueText(value.get_ref<const std::string&>(), profile);
        return leaves != nullptr ? leaves->redact(text) : text;
    }
    const auto text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (value.is_structured()) {
        return redactSensitiveBodyFields(text, profile, leaves)
            .value_or(std::string{kRedactedValueMarker});
    }
    return text;
}

}  // namespace

std::optional<std::string> redactSensitiveBodyFields(std::string_view body,
                                                     const MaskProfile profile,
                                                     const SecretRegistry* registry) {
    if (!isSafeText(body)) {
        return std::nullopt;
    }
    // The JSON walk and dump recurse once per level: a body nested past the limit gets the text
    // scan instead, so a hostile response cannot overflow the stack.
    std::string numbersMasked;  // outlives every use of `body` below
    if (looksLikeJson(body) && !jsonNestingExceeds(body)) {
        if (registry != nullptr && profile == MaskProfile::Agent) {
            numbersMasked = maskSecretNumberLiterals(body, *registry);
            body = numbersMasked;
        }
        auto doc = ordered_json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (!doc.is_discarded()) {
            redactJsonNode(doc, profile, registry);
            return doc.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
        }
    }
    auto text = redactKeyValueText(body, profile);
    if (registry != nullptr && profile == MaskProfile::Agent && looksLikeJson(body)) {
        return redactJsonLiterals(text, *registry);
    }
    return registry != nullptr ? registry->redact(text) : text;
}

std::optional<std::string> safeBodyExcerpt(std::string_view body,
                                           const SecretRegistry* registry,
                                           const std::size_t maxBytes,
                                           const MaskProfile profile) {
    const auto limit = std::min(maxBytes, kMaxFailureExcerptBytes);
    if (body.empty() || limit == 0) {
        return std::nullopt;
    }
    const bool agent = profile == MaskProfile::Agent;
    std::optional<std::string> fields{};
    try {
        // The agent profile value-redacts string values before the dump, never keys.
        fields = redactSensitiveBodyFields(body, profile, agent ? registry : nullptr);
    } catch (...) {
        // Fail closed: a body we could not process is not quoted.
        return std::nullopt;
    }
    if (!fields || fields->empty()) {
        return std::nullopt;
    }
    // Value-redact the whole text before cutting, so a secret straddling
    // the cut cannot leave a recognisable prefix behind.
    std::string masked =
        registry != nullptr && !agent ? registry->redact(*fields) : std::move(*fields);
    return capExcerpt(std::move(masked), limit, body.size());
}

std::optional<std::string> selectedBodyExcerpt(std::string_view body,
                                               const std::vector<std::string>& paths,
                                               const SecretRegistry* registry,
                                               const std::size_t maxBytes,
                                               const MaskProfile profile) {
    const auto limit = std::min(maxBytes, kMaxFailureExcerptBytes);
    if (paths.empty() || limit == 0 || !looksLikeJson(body) || !isSafeText(body) ||
        jsonNestingExceeds(body)) {
        return std::nullopt;
    }
    const auto doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        return std::nullopt;
    }
    std::string text{};
    try {
        for (const auto& path : paths) {
            if (const auto* value = walkJsonPath(doc, path)) {
                text += (text.empty() ? "" : "; ") + path + ": " +
                        maskedSelection(path, *value, profile, registry);
            }
        }
    } catch (...) {
        // Fail closed: the caller falls back to the default excerpt.
        return std::nullopt;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (registry != nullptr) {
        text = registry->redact(text);
    }
    return capExcerpt(std::move(text), limit, body.size());
}

std::optional<std::string> maskCapturedBody(std::string_view body, const RunContext& ctx) {
    std::optional<std::string> fields{};
    try {
        fields = redactSensitiveBodyFields(body, MaskProfile::Agent, &ctx.secretRegistry());
    } catch (...) {
        return std::nullopt;
    }
    return fields;
}

void redactEvent(RunEvent& event, const SecretRegistry& registry) {
    if (registry.empty()) {
        return;
    }
    if (auto* started = std::get_if<RunStarted>(&event)) {
        redactString(started->envName, registry);
    } else if (auto* prepared = std::get_if<RequestPrepared>(&event)) {
        redactString(prepared->url, registry);
        redactHeaderValues(prepared->maskedHeaders, registry);
    } else if (auto* response = std::get_if<ResponseReceived>(&event)) {
        // The captured body is the person's own view and stays as received.
        redactHeaderValues(response->headers, registry);
    } else if (auto* extraction = std::get_if<ExtractionCompleted>(&event)) {
        redactString(extraction->value, registry);
        redactString(extraction->sourcePath, registry);
    } else if (auto* assertion = std::get_if<AssertionCompleted>(&event)) {
        redactString(assertion->name, registry);
        redactString(assertion->expr, registry);
        redactString(assertion->detail, registry);
    } else if (auto* failed = std::get_if<StepFailed>(&event)) {
        redactString(failed->detail, registry);
        for (auto& d : failed->diagnostics) {
            redactString(d.token, registry);
            redactString(d.useName, registry);
            redactString(d.sourceId, registry);
            redactString(d.sourceField, registry);
        }
    }
    // StepStarted, StepSkipped, ExtractionApplied (names only), StepBlocked,
    // StepCancelled, SessionRefreshed and RunEnded carry no free text.
}

void redactStepResult(StepResult& step, const SecretRegistry& registry) {
    if (registry.empty()) {
        return;
    }
    redactString(step.detail, registry);
    for (auto& assertion : step.assertions) {
        redactString(assertion.name, registry);
        redactString(assertion.expr, registry);
        redactString(assertion.detail, registry);
    }
}

}  // namespace reqloom::engine
