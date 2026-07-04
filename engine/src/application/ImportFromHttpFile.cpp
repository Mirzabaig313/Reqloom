// ImportFromHttpFile — `.http` / `.rest` (REST Client / JetBrains HTTP Client)
// → Project parser. Unlike the JSON importers this is line-based.
//
// path/id helpers mirror the JSON importers rather than share a
// header. Upgrade path: extract a shared ImportSupport.h if the duplication
// across all importers becomes worth removing.

#include "ImportFromHttpFile.h"

#include "../domain/DependencyResolver.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{16} * 1024 * 1024;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalPath(const fs::path& file,
                                                                  const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file, ec);
    if (ec) {
        return std::unexpected(
            invalid("http-file import: cannot canonicalise path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("http-file import: not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("http-file import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(
            invalid("http-file import: path resolves outside the project root (" + rootStr + ")"));
    }
    return canonical;
}

std::string nowIso8601Utc() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

std::string toLowerAscii(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

std::string sanitizeId(std::string_view name, std::string_view fallback) {
    std::string out;
    out.reserve(name.size());
    bool pendingSep = false;
    for (const char c : name) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pendingSep && !out.empty()) {
                out.push_back('_');
            }
            pendingSep = false;
            out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else {
            pendingSep = true;
        }
    }
    if (out.empty()) {
        return std::string{fallback};
    }
    return out;
}

std::optional<HttpMethod> parseMethodStrict(std::string_view token) {
    const auto m = toLowerAscii(token);
    if (m == "get") {
        return HttpMethod::Get;
    }
    if (m == "post") {
        return HttpMethod::Post;
    }
    if (m == "put") {
        return HttpMethod::Put;
    }
    if (m == "patch") {
        return HttpMethod::Patch;
    }
    if (m == "delete") {
        return HttpMethod::Delete;
    }
    if (m == "head") {
        return HttpMethod::Head;
    }
    if (m == "options") {
        return HttpMethod::Options;
    }
    return std::nullopt;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Rewrite `{{var}}` references to `{{env.var}}`; already-scoped names pass
/// through. REST Client dynamic values like `{{$guid}}` are left verbatim.
std::string rewriteTokens(std::string_view in, std::set<std::string>& referenced) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto open = in.find("{{", i);
        if (open == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, open - i));
        const auto close = in.find("}}", open + 2);
        if (close == std::string_view::npos) {
            out.append(in.substr(open));
            break;
        }
        std::string_view inner = trim(in.substr(open + 2, close - (open + 2)));
        if (inner.empty()) {
            out.append("{{}}");
        } else if (inner.front() == '$') {
            out.append("{{").append(inner).append("}}");  // dynamic, leave as-is
        } else if (inner.find('.') != std::string_view::npos) {
            out.append("{{").append(inner).append("}}");
        } else {
            referenced.emplace(inner);
            out.append("{{env.").append(inner).append("}}");
        }
        i = close + 2;
    }
    return out;
}

struct SplitUrl {
    std::string base;
    std::string path;
};

SplitUrl splitRawUrl(std::string_view raw, const std::map<std::string, std::string>& vars) {
    auto end = raw.find_first_of("?#");
    std::string_view noQuery = raw.substr(0, end == std::string_view::npos ? raw.size() : end);

    SplitUrl out;
    if (noQuery.starts_with("{{")) {
        const auto close = noQuery.find("}}");
        if (close != std::string_view::npos) {
            const std::string_view name = trim(noQuery.substr(2, close - 2));
            if (const auto it = vars.find(std::string{name}); it != vars.end()) {
                out.base = it->second;
            }
            std::string_view rest = noQuery.substr(close + 2);
            out.path = rest.empty() ? "/" : std::string{rest};
        }
    } else if (const auto scheme = noQuery.find("://"); scheme != std::string_view::npos) {
        const auto authStart = scheme + 3;
        const auto slash = noQuery.find('/', authStart);
        if (slash == std::string_view::npos) {
            out.base = std::string{noQuery};
            out.path = "/";
        } else {
            out.base = std::string{noQuery.substr(0, slash)};
            out.path = std::string{noQuery.substr(slash)};
        }
    } else {
        out.path = std::string{noQuery};
    }

    if (out.path.empty()) {
        out.path = "/";
    } else if (out.path.front() != '/') {
        out.path.insert(out.path.begin(), '/');
    }
    return out;
}

/// One parsed request block.
struct Block {
    std::string name;
    std::string requestLine;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool hasRequestLine{false};
};

}  // namespace

std::expected<ImportFromHttpFile::Outcome, ReqloomError> ImportFromHttpFile::run(
    const fs::path& file, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(file, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(invalid("http-file import: cannot stat file: " + ec.message()));
    }
    if (size > kMaxFileBytes) {
        return std::unexpected(invalid(std::format(
            "http-file import: file is too large ({} bytes; limit {})", size, kMaxFileBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("http-file import: cannot open file"));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // Split into lines (LF; trailing CR trimmed by `trim` at use sites).
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto nl = text.find('\n', start);
            if (nl == std::string::npos) {
                lines.emplace_back(text.substr(start));
                break;
            }
            lines.emplace_back(text.substr(start, nl - start));
            start = nl + 1;
        }
    }

    std::map<std::string, std::string> fileVars;  // @name = value
    std::vector<Block> blocks;
    Block current;

    // Parser state within a block: header phase (before the blank line that
    // ends headers) vs body phase.
    enum class Phase : std::uint8_t { BeforeRequest, Headers, Body };
    Phase phase = Phase::BeforeRequest;
    std::set<std::string> referencedVars;

    auto finalizeBlock = [&]() {
        if (current.hasRequestLine) {
            // Trim a trailing run of blank lines from the body.
            while (!current.body.empty() &&
                   (current.body.back() == '\n' || current.body.back() == '\r' ||
                    current.body.back() == ' ' || current.body.back() == '\t')) {
                current.body.pop_back();
            }
            blocks.push_back(std::move(current));
        }
        current = Block{};
        phase = Phase::BeforeRequest;
    };

    for (const auto& rawLine : lines) {
        const std::string_view line = rawLine;
        const std::string_view t = trim(line);

        // Request separator.
        if (t.starts_with("###")) {
            finalizeBlock();
            current.name = std::string{trim(t.substr(3))};
            continue;
        }

        // Variable definition: @name = value (only meaningful outside a body).
        if (phase != Phase::Body && t.starts_with("@")) {
            const std::string_view rest = t.substr(1);
            const auto eq = rest.find('=');
            if (eq != std::string_view::npos) {
                const std::string key{trim(rest.substr(0, eq))};
                const std::string value{trim(rest.substr(eq + 1))};
                if (!key.empty()) {
                    fileVars[key] = value;
                }
            }
            continue;
        }

        // Comments. `# @name X` / `// @name X` set the block name.
        if (phase != Phase::Body && (t.starts_with("#") || t.starts_with("//"))) {
            const std::string_view afterHash =
                t.starts_with("//") ? trim(t.substr(2)) : trim(t.substr(1));
            if (afterHash.starts_with("@name")) {
                current.name = std::string{trim(afterHash.substr(5))};
            }
            continue;
        }

        if (phase == Phase::BeforeRequest) {
            if (t.empty()) {
                continue;  // skip leading blank lines
            }
            current.requestLine = std::string{t};
            current.hasRequestLine = true;
            phase = Phase::Headers;
            continue;
        }

        if (phase == Phase::Headers) {
            if (t.empty()) {
                phase = Phase::Body;
                continue;
            }
            const auto colon = t.find(':');
            if (colon != std::string_view::npos) {
                const std::string hk{trim(t.substr(0, colon))};
                const std::string hv{trim(t.substr(colon + 1))};
                if (!hk.empty()) {
                    current.headers.emplace_back(hk, hv);
                }
            }
            continue;
        }

        // Body phase: keep raw lines verbatim (preserve original, not trimmed).
        current.body.append(rawLine);
        current.body.push_back('\n');
    }
    finalizeBlock();

    if (blocks.empty()) {
        return std::unexpected(
            invalid("http-file import: no requests found (expected `METHOD url` blocks)"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;

    const std::string stem = pathOr->stem().string();
    outcome.project.name = stem.empty() ? "Imported Requests" : stem;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
    };

    // Seed the environment from @vars (rewriting nested references).
    for (const auto& [key, value] : fileVars) {
        env[key] = rewrite(value);
    }

    const std::string resourceId = sanitizeId(stem, "requests");
    auto& resource = outcome.project.resources[ResourceId{resourceId}];
    resource.id = ResourceId{resourceId};

    const auto importedAt = nowIso8601Utc();
    std::set<std::string> seenOpIds;
    std::string projectBase;
    bool multipleBasesWarned = false;

    for (const auto& block : blocks) {
        // Request line: optional METHOD then URL. Fold is not supported beyond a
        // single line (rare in practice).
        std::string_view rl = block.requestLine;
        HttpMethod method = HttpMethod::Get;
        std::string_view urlPart = rl;
        const auto sp = rl.find_first_of(" \t");
        if (sp != std::string_view::npos) {
            if (const auto m = parseMethodStrict(rl.substr(0, sp))) {
                method = *m;
                urlPart = trim(rl.substr(sp + 1));
                // Drop a trailing " HTTP/1.1" version token if present.
                const auto httpPos = urlPart.rfind(" HTTP/");
                if (httpPos != std::string_view::npos) {
                    urlPart = trim(urlPart.substr(0, httpPos));
                }
            }
        }
        const std::string rawUrl{urlPart};

        std::string opName = sanitizeId(block.name, "");
        if (opName.empty()) {
            opName = sanitizeId(
                std::format(
                    "{}_{}", toLowerAscii(std::string{urlPart.substr(0, 40)}), blocks.size()),
                "request");
        }
        std::string unique = opName;
        int suffix = 2;
        while (resource.operations.contains(unique) ||
               seenOpIds.contains(std::format("{}.{}", resourceId, unique))) {
            unique = std::format("{}_{}", opName, suffix++);
        }
        seenOpIds.insert(std::format("{}.{}", resourceId, unique));

        Operation op;
        op.id = OperationId{std::format("{}.{}", resourceId, unique)};
        op.resource = ResourceId{resourceId};
        op.method = method;

        const auto split = splitRawUrl(rawUrl, env);
        if (!split.base.empty()) {
            if (projectBase.empty()) {
                projectBase = split.base;
            } else if (projectBase != split.base && !multipleBasesWarned) {
                warnings.push_back(std::format(
                    "requests target more than one host; the project baseUrl was set to `{}` — "
                    "review operations that use a different host",
                    projectBase));
                multipleBasesWarned = true;
            }
        }
        op.pathTemplate = rewrite(split.path);

        // Query params from the URL's `?a=b&c=d` portion.
        if (const auto q = rawUrl.find('?'); q != std::string::npos) {
            std::string_view query = rawUrl;
            query.remove_prefix(q + 1);
            if (const auto frag = query.find('#'); frag != std::string_view::npos) {
                query = query.substr(0, frag);
            }
            std::size_t start = 0;
            while (start <= query.size()) {
                const auto amp = query.find('&', start);
                const std::string_view pair = query.substr(
                    start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
                if (!pair.empty()) {
                    const auto eq = pair.find('=');
                    const std::string k{eq == std::string_view::npos ? pair : pair.substr(0, eq)};
                    const std::string v{eq == std::string_view::npos ? std::string_view{}
                                                                     : pair.substr(eq + 1)};
                    if (!k.empty()) {
                        op.queryParams[k] = rewrite(v);
                    }
                }
                if (amp == std::string_view::npos) {
                    break;
                }
                start = amp + 1;
            }
        }

        for (const auto& [hk, hv] : block.headers) {
            op.headers[hk] = rewrite(hv);
        }

        if (!block.body.empty()) {
            op.bodyTemplate = rewrite(block.body);
        }

        Provenance prov;
        prov.source = Provenance::Source::HandWritten;  // no .http-specific enum value
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        prov.evidence["imported_from"] = "http_file";
        if (!block.name.empty()) {
            prov.evidence["http_name"] = block.name;
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    }

    if (!projectBase.empty()) {
        env["baseUrl"] = projectBase;
    } else if (!env.contains("baseUrl")) {
        warnings.push_back(
            "no host could be derived from the requests — set `baseUrl` in the environment");
    }

    for (const auto& ref : referencedVars) {
        if (!env.contains(ref)) {
            env[ref] = "";
            warnings.push_back(std::format(
                "variable `{{{{{}}}}}` is referenced but was not defined — set it in the "
                "environment",
                ref));
        }
    }

    if (auto valid = DependencyResolver{}.validate(outcome.project); !valid) {
        return std::unexpected(valid.error());
    }

    std::ostringstream wbuf;
    for (const auto& w : warnings) {
        wbuf << w << '\n';
    }
    outcome.warnings = wbuf.str();
    return outcome;
}

}  // namespace reqloom::engine
