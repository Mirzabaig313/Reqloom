// JsonExtraction — see header.
#include "JsonExtraction.h"

#include "Cookies.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace chainapi::engine {

namespace {
using json = nlohmann::json;

constexpr std::size_t kMaxTraceValueBytes = 256;

std::string truncateForTrace(std::string s) {
    if (s.size() <= kMaxTraceValueBytes) return s;
    s.resize(kMaxTraceValueBytes);
    s += "...";
    return s;
}

[[nodiscard]] std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

}  // namespace

std::expected<std::map<std::string, std::string>, ChainApiError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions) {
    if (extractions.empty()) return std::map<std::string, std::string>{};

    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected(
            ChainApiError{ErrorCode::ResponseParse,
                          ErrorClass::Extraction,
                          std::string("response is not valid JSON: ") + e.what()});
    }

    // Walk a single segment that may be "name", "name[N]", or "[N]".
    // Returns nullptr on miss.
    auto walkSegment = [](const json* current, const std::string& segment) -> const json* {
        if (segment.empty()) return current;

        const auto bracketPos = segment.find('[');
        const std::string name =
            (bracketPos == std::string::npos) ? segment : segment.substr(0, bracketPos);

        if (!name.empty()) {
            if (!current->is_object()) return nullptr;
            auto it = current->find(name);
            if (it == current->end()) return nullptr;
            current = &(*it);
        }

        // Apply each [N] index in turn (e.g. data[0][1]).
        std::size_t pos = bracketPos;
        while (pos != std::string::npos) {
            const auto closePos = segment.find(']', pos);
            if (closePos == std::string::npos) return nullptr;
            const auto indexStr = segment.substr(pos + 1, closePos - pos - 1);
            std::size_t index = 0;
            const auto* first = indexStr.data();
            const auto* last = first + indexStr.size();
            const auto fc = std::from_chars(first, last, index);
            // Reject partial parses — `[0xFF]` would otherwise parse as
            // 0 and silently alias to the first element.
            if (fc.ec != std::errc{} || fc.ptr != last) return nullptr;

            if (!current->is_array() || index >= current->size()) return nullptr;
            current = &(*current)[index];

            pos = segment.find('[', closePos);
        }
        return current;
    };

    std::map<std::string, std::string> values;
    for (const auto& ext : extractions) {
        auto path = ext.sourcePath;
        if (path.starts_with("$.")) path = path.substr(2);

        const json* current = &doc;
        bool found = true;
        std::istringstream ss(path);
        std::string segment;
        while (std::getline(ss, segment, '.')) {
            current = walkSegment(current, segment);
            if (!current) {
                found = false;
                break;
            }
        }
        if (!found) {
            return std::unexpected(
                ChainApiError{ErrorCode::ExtractionFailed,
                              ErrorClass::Extraction,
                              "extract path '" + ext.sourcePath +
                                  "' not found in response (variable: " + ext.variableName + ")"});
        }

        std::string value = current->is_string() ? current->get<std::string>() : current->dump();
        values[ext.variableName] = std::move(value);
    }
    return values;
}

namespace {

const json* walkPathOrNull(const json& doc, std::string_view sourcePath) {
    std::string path{sourcePath};
    if (path.starts_with("$.")) path = path.substr(2);

    const json* current = &doc;
    std::istringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
        if (segment.empty()) continue;
        const auto bracketPos = segment.find('[');
        const std::string name =
            (bracketPos == std::string::npos) ? segment : segment.substr(0, bracketPos);

        if (!name.empty()) {
            if (!current->is_object()) return nullptr;
            auto it = current->find(name);
            if (it == current->end()) return nullptr;
            current = &(*it);
        }

        std::size_t pos = bracketPos;
        while (pos != std::string::npos) {
            const auto closePos = segment.find(']', pos);
            if (closePos == std::string::npos) return nullptr;
            const auto indexStr = segment.substr(pos + 1, closePos - pos - 1);
            std::size_t index = 0;
            const auto* first = indexStr.data();
            const auto* last = first + indexStr.size();
            const auto fc = std::from_chars(first, last, index);
            // Reject partial parses — `[0xFF]` would otherwise parse as
            // 0 and silently alias to the first element.
            if (fc.ec != std::errc{} || fc.ptr != last) return nullptr;
            if (!current->is_array() || index >= current->size()) return nullptr;
            current = &(*current)[index];
            pos = segment.find('[', closePos);
        }
    }
    return current;
}

/// Strip a leading `$.<prefix>.` from sourcePath if present, returning
/// the remainder. Used by Header / Cookie sources where the schema
/// convention is `$.headers.X` or `$.cookies.X`.
[[nodiscard]] std::string stripPrefix(std::string_view sourcePath, std::string_view prefix) {
    if (sourcePath.starts_with(prefix)) {
        return std::string{sourcePath.substr(prefix.size())};
    }
    return std::string{sourcePath};
}

/// Look up a header by name, case-insensitive per RFC 7230 §3.2.
/// Returns nullopt when not present.
[[nodiscard]] std::optional<std::string> findHeader(
    const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
    const auto target = toLowerCopy(name);
    for (const auto& [k, v] : headers) {
        if (toLowerCopy(k) == target) return v;
    }
    return std::nullopt;
}

/// Parse a `Set-Cookie` header value and return the named cookie's
/// value, or nullopt when absent. Delegates to the shared cookies
/// parser — same shape used by the executor's per-actor jar.
[[nodiscard]] std::optional<std::string> parseSetCookieValue(std::string_view header,
                                                             std::string_view cookieName) {
    auto parsed = cookies::parseSetCookie(header);
    if (!parsed) return std::nullopt;
    if (parsed->first != cookieName) return std::nullopt;
    return std::move(parsed->second);
}

/// Look up a cookie across all `Set-Cookie` headers present on the
/// response. Returns the value of the LAST matching cookie when more
/// than one collides — RFC 6265 §5.3 step 11 says a newer cookie
/// replaces an older one in the user-agent's store, and that's the
/// behavior browsers and curl ship. Pinning "last wins" here makes
/// schemas portable: the same response that works in a browser
/// resolves the same way through the engine.
[[nodiscard]] std::optional<std::string> findCookie(
    const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
    std::optional<std::string> latest;
    for (const auto& [k, v] : headers) {
        if (toLowerCopy(k) != "set-cookie") continue;
        if (auto found = parseSetCookieValue(v, name); found) latest = std::move(found);
    }
    return latest;
}

/// Run a regex against the body and return capture group 1 if present,
/// otherwise the whole match. Returns nullopt on no match.
///
/// Catches std::regex_error so a malformed pattern surfaces as Missing
/// rather than blowing up the whole step. Hook authors can spot pattern
/// issues in the trace value (which is empty on Missing) without
/// crashing the run.
[[nodiscard]] std::optional<std::string> findRegex(std::string_view body,
                                                   std::string_view pattern) {
    try {
        const std::regex re{std::string{pattern}};
        std::cmatch match;
        const std::string bodyStr{body};
        if (!std::regex_search(bodyStr.c_str(), match, re)) return std::nullopt;
        if (match.size() >= 2 && match[1].matched) return match[1].str();
        return match[0].str();
    } catch (const std::regex_error&) {
        return std::nullopt;
    }
}

/// Walk a JSONPath against an already-parsed JSON document and append
/// the trace. Shared by `extractFromJsonDetailed` and the new
/// response-aware overload.
void resolveJsonPath(const json& doc,
                     const Extraction& ext,
                     ExtractionTrace& trace,
                     DetailedExtraction& out) {
    const auto* node = walkPathOrNull(doc, ext.sourcePath);
    if (node == nullptr) {
        trace.outcome = ExtractionTrace::Outcome::Missing;
        return;
    }
    if (node->is_null()) {
        trace.outcome = ExtractionTrace::Outcome::Null;
        return;
    }
    std::string value = node->is_string() ? node->get<std::string>() : node->dump();
    trace.value = truncateForTrace(value);
    trace.outcome = ExtractionTrace::Outcome::Resolved;
    out.values[ext.variableName] = std::move(value);
}

/// Whether at least one extraction in the list needs the JSON body.
[[nodiscard]] bool anyNeedsJsonParse(const std::vector<Extraction>& extractions) {
    for (const auto& ext : extractions) {
        if (ext.source == Extraction::Source::JsonPath) return true;
    }
    return false;
}

}  // namespace

std::expected<DetailedExtraction, ChainApiError> extractFromJsonDetailed(
    const OperationId& opId, const std::string& body, const std::vector<Extraction>& extractions) {
    // Body-only entry point: zero status code and no headers means
    // Header / StatusCode / Cookie / Regex all surface as Missing.
    return extractFromResponseDetailed(opId, body, 0, {}, extractions);
}

std::expected<DetailedExtraction, ChainApiError> extractFromResponseDetailed(
    const OperationId& opId,
    const std::string& body,
    int statusCode,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<Extraction>& extractions) {
    DetailedExtraction out;
    if (extractions.empty()) return out;

    json doc;
    bool docParsed = false;
    if (anyNeedsJsonParse(extractions)) {
        try {
            doc = json::parse(body);
            docParsed = true;
        } catch (const json::parse_error& e) {
            return std::unexpected(
                ChainApiError{ErrorCode::ResponseParse,
                              ErrorClass::Extraction,
                              std::string("response is not valid JSON: ") + e.what()});
        }
    }

    out.traces.reserve(extractions.size());
    for (const auto& ext : extractions) {
        ExtractionTrace trace;
        trace.op = opId;
        trace.variableName = ext.variableName;
        trace.sourcePath = ext.sourcePath;
        trace.sourceKind = ext.source;

        switch (ext.source) {
            case Extraction::Source::JsonPath: {
                if (!docParsed) {
                    // Defensive — we said anyNeedsJsonParse was false yet
                    // a JsonPath slipped through. Mark as Missing rather
                    // than UB.
                    trace.outcome = ExtractionTrace::Outcome::Missing;
                    break;
                }
                resolveJsonPath(doc, ext, trace, out);
                break;
            }
            case Extraction::Source::Header: {
                const auto headerName = stripPrefix(ext.sourcePath, "$.headers.");
                auto value = findHeader(headers, headerName);
                if (!value) {
                    trace.outcome = ExtractionTrace::Outcome::Missing;
                    break;
                }
                trace.value = truncateForTrace(*value);
                trace.outcome = ExtractionTrace::Outcome::Resolved;
                out.values[ext.variableName] = std::move(*value);
                break;
            }
            case Extraction::Source::StatusCode: {
                if (statusCode == 0) {
                    // No status was passed in (body-only call site).
                    trace.outcome = ExtractionTrace::Outcome::Missing;
                    break;
                }
                std::string value = std::to_string(statusCode);
                trace.value = value;
                trace.outcome = ExtractionTrace::Outcome::Resolved;
                out.values[ext.variableName] = std::move(value);
                break;
            }
            case Extraction::Source::Cookie: {
                const auto cookieName = stripPrefix(ext.sourcePath, "$.cookies.");
                auto value = findCookie(headers, cookieName);
                if (!value) {
                    trace.outcome = ExtractionTrace::Outcome::Missing;
                    break;
                }
                trace.value = truncateForTrace(*value);
                trace.outcome = ExtractionTrace::Outcome::Resolved;
                out.values[ext.variableName] = std::move(*value);
                break;
            }
            case Extraction::Source::Regex: {
                auto value = findRegex(body, ext.sourcePath);
                if (!value) {
                    trace.outcome = ExtractionTrace::Outcome::Missing;
                    break;
                }
                trace.value = truncateForTrace(*value);
                trace.outcome = ExtractionTrace::Outcome::Resolved;
                out.values[ext.variableName] = std::move(*value);
                break;
            }
            case Extraction::Source::XPath:
                // XML parsing is post-MVP — flag for the user instead of
                // silently failing. The trace value stays empty.
                trace.outcome = ExtractionTrace::Outcome::Unsupported;
                break;
        }

        out.traces.push_back(std::move(trace));
    }
    return out;
}

}  // namespace chainapi::engine
