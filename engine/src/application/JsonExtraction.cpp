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
#include <regex>
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
    for (const char c : expr) {
        if (quote != 0) {
            if (c == quote) {
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

/// Walk a JSONPath against a parsed document. Supports field access
/// (`a.b`), array indexing (`a[0]`, `a[0][1]`), and predicate filters
/// (`a[?(@.field == 'x')]`, first match wins — `@` is the array element).
/// Returns nullptr on any miss. Bracket-aware tokeniser so a `.` inside a
/// filter (e.g. `@.status`) doesn't split the path.
[[nodiscard]] const json* walkJson(const json& doc, std::string_view sourcePath) {
    std::string path{sourcePath};
    std::size_t i = 0;
    const std::size_t n = path.size();
    if (i < n && path[i] == '$') {
        ++i;
    }
    if (i < n && path[i] == '.') {
        ++i;
    }

    static const PredicateEvaluator evaluator;
    const json* current = &doc;
    while (i < n) {
        // Field name up to the next '.' or '['.
        std::string name;
        while (i < n && path[i] != '.' && path[i] != '[') {
            name.push_back(path[i]);
            ++i;
        }
        if (!name.empty()) {
            if (!current->is_object()) {
                return nullptr;
            }
            const auto it = current->find(name);
            if (it == current->end()) {
                return nullptr;
            }
            current = &(*it);
        }

        // Zero or more bracket segments: [N] index or [?(...)] filter.
        while (i < n && path[i] == '[') {
            // Find the matching ']' while skipping over quoted strings.
            std::size_t j = i + 1;
            char quote = 0;
            while (j < n) {
                const char c = path[j];
                if (quote != 0) {
                    if (c == quote) {
                        quote = 0;
                    }
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == ']') {
                    break;
                }
                ++j;
            }
            if (j >= n) {
                return nullptr;  // unmatched '['
            }
            const std::string inside = path.substr(i + 1, j - (i + 1));

            if (inside.starts_with("?(") && inside.ends_with(")")) {
                if (!current->is_array()) {
                    return nullptr;
                }
                const std::string expr =
                    filterAtToDollar(std::string_view{inside}.substr(2, inside.size() - 3));
                auto parsed = evaluator.parse(expr);
                if (!parsed) {
                    return nullptr;  // malformed filter
                }
                const json* match = nullptr;
                for (const auto& element : *current) {
                    if (evaluator.evaluate(*parsed, element.dump()) == PredicateValue::True) {
                        match = &element;
                        break;
                    }
                }
                if (match == nullptr) {
                    return nullptr;
                }
                current = match;
            } else {
                std::size_t index = 0;
                const auto* first = inside.data();
                const auto* last = first + inside.size();
                const auto fc = std::from_chars(first, last, index);
                if (fc.ec != std::errc{} || fc.ptr != last) {
                    return nullptr;
                }
                if (!current->is_array() || index >= current->size()) {
                    return nullptr;
                }
                current = &(*current)[index];
            }
            i = j + 1;
        }

        if (i < n && path[i] == '.') {
            ++i;
        }
    }
    return current;
}

}  // namespace

const json* walkJsonPath(const json& doc, std::string_view sourcePath) {
    return walkJson(doc, sourcePath);
}

namespace {

/// Multi-match walker: like `walkJson` but `[*]` expands to every array
/// element and filters/indices apply across the whole current node set, so a
/// path can resolve to many nodes. Returns pointers into `doc`.
[[nodiscard]] std::vector<const json*> walkJsonMulti(const json& doc, std::string_view sourcePath) {
    const std::string path{sourcePath};
    std::size_t i = 0;
    const std::size_t n = path.size();
    if (i < n && path[i] == '$') {
        ++i;
    }
    if (i < n && path[i] == '.') {
        ++i;
    }

    static const PredicateEvaluator evaluator;
    std::vector<const json*> current{&doc};
    while (i < n && !current.empty()) {
        std::string name;
        while (i < n && path[i] != '.' && path[i] != '[') {
            name.push_back(path[i]);
            ++i;
        }
        if (!name.empty()) {
            std::vector<const json*> next;
            for (const auto* node : current) {
                if (node->is_object()) {
                    const auto it = node->find(name);
                    if (it != node->end()) {
                        next.push_back(&(*it));
                    }
                }
            }
            current = std::move(next);
        }

        while (i < n && path[i] == '[') {
            std::size_t j = i + 1;
            char quote = 0;
            while (j < n) {
                const char c = path[j];
                if (quote != 0) {
                    if (c == quote) {
                        quote = 0;
                    }
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == ']') {
                    break;
                }
                ++j;
            }
            if (j >= n) {
                return {};
            }
            const std::string inside = path.substr(i + 1, j - (i + 1));
            std::vector<const json*> next;
            if (inside == "*") {
                for (const auto* node : current) {
                    if (node->is_array()) {
                        for (const auto& element : *node) {
                            next.push_back(&element);
                        }
                    }
                }
            } else if (inside.starts_with("?(") && inside.ends_with(")")) {
                const std::string expr =
                    filterAtToDollar(std::string_view{inside}.substr(2, inside.size() - 3));
                if (auto parsed = evaluator.parse(expr)) {
                    for (const auto* node : current) {
                        if (!node->is_array()) {
                            continue;
                        }
                        for (const auto& element : *node) {
                            if (evaluator.evaluate(*parsed, element.dump()) ==
                                PredicateValue::True) {
                                next.push_back(&element);
                            }
                        }
                    }
                }
            } else {
                std::size_t index = 0;
                const auto* first = inside.data();
                const auto* last = first + inside.size();
                const auto fc = std::from_chars(first, last, index);
                if (fc.ec == std::errc{} && fc.ptr == last) {
                    for (const auto* node : current) {
                        if (node->is_array() && index < node->size()) {
                            next.push_back(&(*node)[index]);
                        }
                    }
                }
            }
            current = std::move(next);
            i = j + 1;
        }

        if (i < n && path[i] == '.') {
            ++i;
        }
    }
    return current;
}

}  // namespace

std::vector<std::string> evaluateJsonPathAll(const json& doc, std::string_view sourcePath) {
    std::vector<std::string> out;
    for (const auto* node : walkJsonMulti(doc, sourcePath)) {
        out.push_back(node->is_string() ? node->get<std::string>() : node->dump());
    }
    return out;
}

std::vector<std::string> extractValues(const std::string& body, std::string_view sourcePath) {
    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::parse_error&) {
        return {};
    }
    return evaluateJsonPathAll(doc, sourcePath);
}

std::expected<std::map<std::string, std::string>, ReqloomError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions) {
    if (extractions.empty()) {
        return std::map<std::string, std::string>{};
    }

    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected(
            ReqloomError{ErrorCode::ResponseParse,
                         ErrorClass::Extraction,
                         std::string("response is not valid JSON: ") + e.what()});
    }

    // Walk a single segment that may be "name", "name[N]", or "[N]".
    // Returns nullptr on miss.
    std::map<std::string, std::string> values;
    for (const auto& ext : extractions) {
        const json* current = walkJson(doc, ext.sourcePath);
        if (current == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::ExtractionFailed,
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
    return walkJson(doc, sourcePath);
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

/// Result of running a regex extraction. We need to distinguish three
/// outcomes — match found, pattern was syntactically valid but didn't
/// match anything, pattern itself was malformed — so the trace can
/// surface "your pattern is broken" instead of conflating it with
/// "your pattern is fine but the response didn't match".
enum class RegexOutcome : std::uint8_t { Matched, NoMatch, InvalidPattern };

struct RegexResult {
    RegexOutcome outcome{RegexOutcome::NoMatch};
    std::string value;  ///< Populated only when outcome == Matched.
};

/// Run a regex against the body. On a match, returns capture group 1
/// when present, otherwise the whole match.
///
/// Returns `InvalidPattern` (not an exception) when std::regex rejects
/// the pattern as malformed — extraction code maps that to its own
/// trace outcome so the user sees a distinct row in the timeline.
[[nodiscard]] RegexResult findRegex(std::string_view body, std::string_view pattern) {
    try {
        const std::regex re{std::string{pattern}};
        std::match_results<std::string_view::const_iterator> match;
        if (!std::regex_search(body.begin(), body.end(), match, re)) {
            return RegexResult{RegexOutcome::NoMatch, {}};
        }
        if (match.size() >= 2 && match[1].matched) {
            return RegexResult{RegexOutcome::Matched, match[1].str()};
        }
        return RegexResult{RegexOutcome::Matched, match[0].str()};
    } catch (const std::regex_error&) {
        return RegexResult{RegexOutcome::InvalidPattern, {}};
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
        try {
            doc = json::parse(body);
            docParsed = true;
        } catch (const json::parse_error& e) {
            return std::unexpected(
                ReqloomError{ErrorCode::ResponseParse,
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
                const auto rx = findRegex(body, ext.sourcePath);
                switch (rx.outcome) {
                    case RegexOutcome::Matched:
                        trace.value = truncateForTrace(rx.value);
                        trace.outcome = ExtractionTrace::Outcome::Resolved;
                        out.values[ext.variableName] = rx.value;
                        break;
                    case RegexOutcome::NoMatch:
                        // Pattern compiled fine; the body just didn't
                        // match. Same outcome as a JsonPath that walks
                        // the document successfully but finds no node.
                        trace.outcome = ExtractionTrace::Outcome::Missing;
                        break;
                    case RegexOutcome::InvalidPattern:
                        // Pattern itself was malformed — distinct from
                        // Missing so users can fix the schema instead
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
