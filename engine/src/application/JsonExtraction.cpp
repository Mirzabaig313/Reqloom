// JsonExtraction — see header.
#include "JsonExtraction.h"

#include "Cookies.h"
#include "PredicateEvaluator.h"

#include <reqloom/engine/JsonValues.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace reqloom::engine {

namespace {
using json = nlohmann::json;

constexpr std::size_t kMaxTraceValueBytes = 256;

std::string truncateForTrace(std::string s) {
    if (s.size() <= kMaxTraceValueBytes) {
        return s;
    }
    s.resize(kMaxTraceValueBytes);
    s += "...";
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

/// Translate a JSONPath filter expression's `@` (current element) to `$`
/// (predicate root) so it can be evaluated by PredicateEvaluator — but only
/// outside string literals, so a value like 'a@b.com' is left intact.
[[nodiscard]] std::string filterAtToDollar(std::string_view expr) {
    std::string out;
    out.reserve(expr.size());
    char quote = 0;
    bool escaped = false;
    for (const char c : expr) {
        if (quote != 0) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = 0;
            }
            out.push_back(c);
        } else if (c == '\'' || c == '"') {
            quote = c;
            out.push_back(c);
        } else if (c == '@') {
            out.push_back('$');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

struct PathSegment {
    enum class Kind : std::uint8_t { Field, Index, Wildcard, Filter };
    Kind kind{Kind::Field};
    std::string field;
    std::size_t index{};
    ParsedPredicate filter;  ///< Only for Kind::Filter; `@` already mapped to `$`.
};

using PathSegments = std::vector<PathSegment>;

/// Index of the `]` closing the `[` at `open`, skipping nested brackets and quoted strings so
/// a filter like `[?(@.tags[0] == 'x')]` stays whole; npos when unbalanced.
[[nodiscard]] std::size_t findClosingBracket(std::string_view path, std::size_t open) {
    std::size_t depth = 0;
    char quote = 0;
    bool escaped = false;
    for (std::size_t j = open; j < path.size(); ++j) {
        const char c = path[j];
        if (quote != 0) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '[' || c == '(') {
            ++depth;
        } else if (c == ']' || c == ')') {
            if (depth == 0) {
                return std::string_view::npos;
            }
            if (--depth == 0) {
                return c == ']' ? j : std::string_view::npos;
            }
        }
    }
    return std::string_view::npos;
}

/// Classify the text between `[` and `]`: `*`, a non-negative index or a `?(...)` filter;
/// anything else is an error.
[[nodiscard]] std::expected<PathSegment, std::string> parseBracket(std::string_view inside) {
    static const PredicateEvaluator evaluator;
    PathSegment seg;
    if (inside == "*") {
        seg.kind = PathSegment::Kind::Wildcard;
        return seg;
    }
    if (inside.starts_with("?(") && inside.ends_with(")")) {
        auto parsed = evaluator.parse(filterAtToDollar(inside.substr(2, inside.size() - 3)));
        if (!parsed) {
            return std::unexpected("invalid filter [" + std::string{inside} +
                                   "]: " + parsed.error().detail);
        }
        seg.kind = PathSegment::Kind::Filter;
        seg.filter = std::move(*parsed);
        return seg;
    }
    const auto* first = inside.data();
    const auto* last = first + inside.size();
    const auto fc = std::from_chars(first, last, seg.index);
    if (inside.empty() || fc.ec != std::errc{} || fc.ptr != last) {
        return std::unexpected("unsupported bracket expression [" + std::string{inside} +
                               "]; expected an index, [*] or a [?(...)] filter");
    }
    seg.kind = PathSegment::Kind::Index;
    return seg;
}

/// Parse the engine's JSONPath subset: optional `$`, then `.field`, `[N]`, `[*]` and
/// `[?(...)]` segments. Unsupported syntax is an error, never a silent miss.
[[nodiscard]] std::expected<PathSegments, std::string> parsePath(std::string_view path) {
    PathSegments segments;
    std::size_t i = (!path.empty() && path[0] == '$') ? 1 : 0;
    const std::size_t n = path.size();
    if (i == 1 && i + 1 == n && path[i] == '.') {
        return segments;  // `$.` — the whole document
    }
    bool nameExpected = i < n && path[i] != '.' && path[i] != '[';
    while (i < n || nameExpected) {
        if (nameExpected) {
            if (i < n && path[i] == '.') {
                return std::unexpected("recursive descent '..' is not supported (position " +
                                       std::to_string(i) + ")");
            }
            const auto stop = std::min(path.find_first_of(".[", i), n);
            if (stop == i) {
                return std::unexpected("empty field name at position " + std::to_string(i));
            }
            PathSegment seg;
            seg.field = std::string{path.substr(i, stop - i)};
            segments.push_back(std::move(seg));
            i = stop;
            nameExpected = false;
        } else if (path[i] == '.') {
            ++i;
            nameExpected = true;
        } else {
            const auto close = findClosingBracket(path, i);
            if (close == std::string_view::npos) {
                return std::unexpected("unclosed '[' at position " + std::to_string(i));
            }
            auto seg = parseBracket(path.substr(i + 1, close - i - 1));
            if (!seg) {
                return std::unexpected(std::move(seg).error());
            }
            segments.push_back(std::move(*seg));
            i = close + 1;
        }
    }
    return segments;
}

[[nodiscard]] bool filterMatches(const ParsedPredicate& filter, const json& element) {
    static const PredicateEvaluator evaluator;
    return evaluator.evaluate(filter, element.dump()) == PredicateValue::True;
}

/// Single-value walk: a filter selects its first match; nullptr on a miss; `[*]` is an error.
[[nodiscard]] std::expected<const json*, std::string> walkSingle(const json& doc,
                                                                 const PathSegments& segments) {
    const json* current = &doc;
    for (const auto& seg : segments) {
        switch (seg.kind) {
            case PathSegment::Kind::Field: {
                if (!current->is_object()) {
                    return nullptr;
                }
                const auto it = current->find(seg.field);
                if (it == current->end()) {
                    return nullptr;
                }
                current = &(*it);
                break;
            }
            case PathSegment::Kind::Index:
                if (!current->is_array() || seg.index >= current->size()) {
                    return nullptr;
                }
                current = &(*current)[seg.index];
                break;
            case PathSegment::Kind::Wildcard:
                return std::unexpected(
                    std::string{"[*] selects many values; it is only valid in a list extraction"});
            case PathSegment::Kind::Filter: {
                if (!current->is_array()) {
                    return nullptr;
                }
                const auto match = std::ranges::find_if(
                    *current, [&](const json& el) { return filterMatches(seg.filter, el); });
                if (match == current->end()) {
                    return nullptr;
                }
                current = &(*match);
                break;
            }
        }
    }
    return current;
}

/// Multi-match walk: `[*]` expands to every element and filters keep every match.
[[nodiscard]] std::vector<const json*> walkMulti(const json& doc, const PathSegments& segments) {
    std::vector<const json*> current{&doc};
    for (const auto& seg : segments) {
        std::vector<const json*> next;
        for (const auto* node : current) {
            if (seg.kind == PathSegment::Kind::Field) {
                if (node->is_object()) {
                    if (const auto it = node->find(seg.field); it != node->end()) {
                        next.push_back(&(*it));
                    }
                }
            } else if (seg.kind == PathSegment::Kind::Index) {
                if (node->is_array() && seg.index < node->size()) {
                    next.push_back(&(*node)[seg.index]);
                }
            } else if (node->is_array()) {
                for (const auto& element : *node) {
                    if (seg.kind == PathSegment::Kind::Wildcard ||
                        filterMatches(seg.filter, element)) {
                        next.push_back(&element);
                    }
                }
            }
        }
        current = std::move(next);
    }
    return current;
}

[[nodiscard]] std::expected<const json*, std::string> walkJson(const json& doc,
                                                               std::string_view sourcePath) {
    const auto segments = parsePath(sourcePath);
    if (!segments) {
        return std::unexpected(segments.error());
    }
    return walkSingle(doc, *segments);
}

/// Parse a response body for extraction; nesting beyond kMaxJsonNestingDepth is rejected
/// first. Errors report only a byte offset: nlohmann's message embeds an input excerpt that
/// could leak a token.
[[nodiscard]] std::expected<json, ReqloomError> parseResponseJson(const std::string& body) {
    if (jsonNestingExceeds(body)) {
        return std::unexpected(ReqloomError{
            ErrorCode::ResponseParse,
            ErrorClass::Extraction,
            "response JSON nests deeper than " + std::to_string(kMaxJsonNestingDepth) + " levels"});
    }
    try {
        return json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected(ReqloomError{
            ErrorCode::ResponseParse,
            ErrorClass::Extraction,
            "response is not valid JSON (syntax error at byte " + std::to_string(e.byte) + ")"});
    }
}

}  // namespace

const json* walkJsonPath(const json& doc, std::string_view sourcePath) {
    const auto hit = walkJson(doc, sourcePath);
    return hit ? *hit : nullptr;
}

std::vector<std::string> evaluateJsonPathAll(const json& doc, std::string_view sourcePath) {
    std::vector<std::string> out;
    const auto segments = parsePath(sourcePath);
    if (!segments) {
        return out;
    }
    for (const auto* node : walkMulti(doc, *segments)) {
        out.push_back(node->is_string() ? node->get<std::string>() : node->dump());
    }
    return out;
}

std::vector<std::string> extractValues(const std::string& body, std::string_view sourcePath) {
    const auto doc = parseResponseJson(body);
    if (!doc) {
        return {};
    }
    return evaluateJsonPathAll(*doc, sourcePath);
}

std::expected<std::map<std::string, std::string>, ReqloomError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions) {
    if (extractions.empty()) {
        return std::map<std::string, std::string>{};
    }

    const auto doc = parseResponseJson(body);
    if (!doc) {
        return std::unexpected(doc.error());
    }

    std::map<std::string, std::string> values;
    for (const auto& ext : extractions) {
        const auto hit = walkJson(*doc, ext.sourcePath);
        if (!hit) {
            return std::unexpected(ReqloomError{ErrorCode::ExtractionFailed,
                                                ErrorClass::Extraction,
                                                "extract path '" + ext.sourcePath +
                                                    "' is not a supported JSONPath (variable: " +
                                                    ext.variableName + "): " + hit.error()});
        }
        const json* current = *hit;
        if (current == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::ExtractionFailed,
                             ErrorClass::Extraction,
                             "extract path '" + ext.sourcePath +
                                 "' not found in response (variable: " + ext.variableName + ")"});
        }
        // JSON null is a missing value, not the string "null" (which would be sent as `Bearer
        // null`).
        if (current->is_null()) {
            return std::unexpected(
                ReqloomError{ErrorCode::ExtractionFailed,
                             ErrorClass::Extraction,
                             "extract path '" + ext.sourcePath +
                                 "' is null in response (variable: " + ext.variableName + ")"});
        }

        std::string value = current->is_string() ? current->get<std::string>() : current->dump();
        values[ext.variableName] = std::move(value);
    }
    return values;
}

namespace {

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
        if (toLowerCopy(k) == target) {
            return v;
        }
    }
    return std::nullopt;
}

/// Parse a `Set-Cookie` header value and return the named cookie's
/// value, or nullopt when absent. Delegates to the shared cookies
/// parser — same shape used by the executor's per-actor jar.
[[nodiscard]] std::optional<std::string> parseSetCookieValue(std::string_view header,
                                                             std::string_view cookieName) {
    auto parsed = cookies::parseSetCookie(header);
    if (!parsed) {
        return std::nullopt;
    }
    if (parsed->first != cookieName) {
        return std::nullopt;
    }
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
        if (toLowerCopy(k) != "set-cookie") {
            continue;
        }
        if (auto found = parseSetCookieValue(v, name); found) {
            latest = std::move(found);
        }
    }
    return latest;
}

/// Walk a JSONPath against an already-parsed JSON document and append
/// the trace. Shared by `extractFromJsonDetailed` and the new
/// response-aware overload.
void resolveJsonPath(const json& doc,
                     const Extraction& ext,
                     ExtractionTrace& trace,
                     DetailedExtraction& out) {
    const auto hit = walkJson(doc, ext.sourcePath);
    if (!hit) {
        trace.outcome = ExtractionTrace::Outcome::InvalidPattern;
        return;
    }
    const auto* node = *hit;
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
    return std::ranges::any_of(
        extractions, [](const auto& ext) { return ext.source == Extraction::Source::JsonPath; });
}

}  // namespace

std::expected<DetailedExtraction, ReqloomError> extractFromJsonDetailed(
    const OperationId& opId, const std::string& body, const std::vector<Extraction>& extractions) {
    // Body-only entry point: zero status code and no headers means
    // Header / StatusCode / Cookie / Regex all surface as Missing.
    return extractFromResponseDetailed(opId, body, 0, {}, extractions);
}

std::expected<DetailedExtraction, ReqloomError> extractFromResponseDetailed(
    const OperationId& opId,
    const std::string& body,
    int statusCode,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<Extraction>& extractions) {
    DetailedExtraction out;
    if (extractions.empty()) {
        return out;
    }

    json doc;
    bool docParsed = false;
    if (anyNeedsJsonParse(extractions)) {
        auto parsed = parseResponseJson(body);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        doc = std::move(*parsed);
        docParsed = true;
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
                const auto rx = guardedRegexSearch(body, ext.sourcePath);
                switch (rx.outcome) {
                    case RegexSearchResult::Outcome::Matched:
                        trace.value = truncateForTrace(rx.value);
                        trace.outcome = ExtractionTrace::Outcome::Resolved;
                        out.values[ext.variableName] = rx.value;
                        break;
                    case RegexSearchResult::Outcome::NoMatch:
                        // Pattern compiled fine; the body just didn't
                        // match. Same outcome as a JsonPath that walks
                        // the document successfully but finds no node.
                        trace.outcome = ExtractionTrace::Outcome::Missing;
                        break;
                    case RegexSearchResult::Outcome::Refused:
                        // Pattern malformed or refused by the regex guard,
                        // or the body exceeds the subject cap — distinct
                        // from Missing so users can fix the schema instead
                        // of staring at a body wondering why their
                        // capture group didn't fire.
                        trace.outcome = ExtractionTrace::Outcome::InvalidPattern;
                        break;
                }
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

}  // namespace reqloom::engine
