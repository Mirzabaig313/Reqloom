// ImportFromBruno — Bruno collection (directory of `.bru` block-DSL files) →
// Project parser. Directory-based, unlike the single-file importers.
//
// ponytail: the .bru block parser uses naive brace-depth matching to find a
// block's closing `}` — a JSON body string containing an unbalanced brace
// (e.g. "}" inside a string) could miscount. Upgrade path: a string-aware
// scanner. In practice Bruno bodies are balanced JSON, so this holds.

#include "ImportFromBruno.h"

#include "../domain/DependencyResolver.h"

#include <nlohmann/json.hpp>

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
using json = nlohmann::json;

constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{8} * 1024 * 1024;
constexpr std::uintmax_t kMaxTotalBytes = std::uintmax_t{64} * 1024 * 1024;
constexpr int kMaxFiles = 5000;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

/// Verify `dir` resolves to a directory under `projectRoot` (separator
/// boundary so /proj doesn't admit /proj-evil), blocking `..` escape.
[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalDir(const fs::path& dir,
                                                                 const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(dir, ec);
    if (ec) {
        return std::unexpected(
            invalid("bruno import: cannot canonicalise collection path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_directory(canonical, ec)) {
        return std::unexpected(
            invalid("bruno import: collection root is not a directory: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("bruno import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "bruno import: collection path resolves outside the project root (" + rootStr + ")"));
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

std::optional<HttpMethod> methodFromBlockName(std::string_view name) {
    if (name == "get") {
        return HttpMethod::Get;
    }
    if (name == "post") {
        return HttpMethod::Post;
    }
    if (name == "put") {
        return HttpMethod::Put;
    }
    if (name == "patch") {
        return HttpMethod::Patch;
    }
    if (name == "delete") {
        return HttpMethod::Delete;
    }
    if (name == "head") {
        return HttpMethod::Head;
    }
    if (name == "options") {
        return HttpMethod::Options;
    }
    return std::nullopt;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

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

/// One top-level `.bru` block: `name[:subtype] { inner }`.
struct BruBlock {
    std::string name;
    std::string subtype;
    std::string inner;
};

/// Extract top-level blocks from a `.bru` document via brace-depth matching.
std::vector<BruBlock> parseBlocks(std::string_view text) {
    std::vector<BruBlock> blocks;
    std::size_t i = 0;
    while (i < text.size()) {
        // Find the next block header: identifier chars then optional ':sub'.
        while (i < text.size() &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
        const std::size_t nameStart = i;
        while (i < text.size()) {
            const char c = text[i];
            const bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':';
            if (!ident) {
                break;
            }
            ++i;
        }
        std::string_view header = text.substr(nameStart, i - nameStart);
        // Skip whitespace to the opening brace.
        while (i < text.size() &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
        if (header.empty() || i >= text.size() || text[i] != '{') {
            // Not a block header; advance to avoid an infinite loop.
            if (i < text.size()) {
                ++i;
            }
            continue;
        }
        // Match the block body via brace depth.
        std::size_t depth = 0;
        const std::size_t bodyStart = i + 1;
        std::size_t j = i;
        for (; j < text.size(); ++j) {
            if (text[j] == '{') {
                ++depth;
            } else if (text[j] == '}') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (depth != 0) {
            break;  // unbalanced — stop parsing
        }
        BruBlock block;
        const auto colon = header.find(':');
        if (colon == std::string_view::npos) {
            block.name = std::string{header};
        } else {
            block.name = std::string{header.substr(0, colon)};
            block.subtype = std::string{header.substr(colon + 1)};
        }
        block.inner = std::string{text.substr(bodyStart, j - bodyStart)};
        blocks.push_back(std::move(block));
        i = j + 1;
    }
    return blocks;
}

/// Parse a dictionary block ("key: value" per line) into ordered pairs.
std::vector<std::pair<std::string, std::string>> parseDict(std::string_view inner) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    while (start <= inner.size()) {
        const auto nl = inner.find('\n', start);
        const std::string_view line = trim(inner.substr(
            start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
        if (!line.empty() && !line.starts_with("//")) {
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                const std::string key{trim(line.substr(0, colon))};
                const std::string value{trim(line.substr(colon + 1))};
                if (!key.empty()) {
                    out.emplace_back(key, value);
                }
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

std::string jsonStr(const json& node, std::string_view key) {
    if (node.is_object()) {
        if (const auto it = node.find(key); it != node.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return {};
}

std::string readFile(const fs::path& p, std::error_code& ec) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        ec = std::make_error_code(std::errc::io_error);
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::expected<ImportFromBruno::Outcome, ReqloomError> ImportFromBruno::run(
    const fs::path& input, const fs::path& projectRoot) const {
    // Resolve the collection root from whatever was pointed at.
    std::error_code ec;
    fs::path root;
    if (fs::is_directory(input, ec)) {
        root = input;
    } else if (fs::is_regular_file(input, ec)) {
        root = input.parent_path();
    } else {
        return std::unexpected(invalid("bruno import: input does not exist"));
    }

    const auto rootOr = canonicalDir(root, projectRoot);
    if (!rootOr) {
        return std::unexpected(rootOr.error());
    }
    const fs::path& collectionRoot = *rootOr;

    Outcome outcome;
    std::vector<std::string> warnings;
    std::set<std::string> referencedVars;
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
    };

    // Collection name from bruno.json when present, else the directory name.
    std::string collectionName;
    {
        const auto brunoJson = collectionRoot / "bruno.json";
        if (fs::is_regular_file(brunoJson, ec)) {
            const auto txt = readFile(brunoJson, ec);
            if (!ec) {
                try {
                    collectionName = jsonStr(json::parse(txt), "name");
                } catch (const json::parse_error&) {
                    // Non-fatal: fall back to the directory name.
                }
            }
        }
    }
    if (collectionName.empty()) {
        collectionName = collectionRoot.filename().string();
    }
    outcome.project.name = collectionName.empty() ? "Imported Collection" : collectionName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    const std::string defaultResource = sanitizeId(collectionName, "requests");
    const auto importedAt = nowIso8601Utc();
    std::set<std::string> seenOpIds;
    std::string projectBase;
    bool multipleBasesWarned = false;

    // Walk the tree. `.bru` files under `environments/` seed environments; other
    // request `.bru` files become operations. The top-level sub-directory of a
    // request file is its resource.
    std::uintmax_t totalBytes = 0;
    int fileCount = 0;
    fs::recursive_directory_iterator it(collectionRoot, fs::directory_options::none, ec);
    if (ec) {
        return std::unexpected(invalid("bruno import: cannot walk collection: " + ec.message()));
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (++fileCount > kMaxFiles) {
            warnings.push_back("collection has too many files; some were skipped");
            break;
        }
        const fs::path& entry = it->path();
        // Skip symlinks: recursive_directory_iterator doesn't descend into
        // symlinked dirs by default, but it still yields file symlinks, and
        // reading one could pull content from outside the collection root
        // (e.g. `x.bru -> /etc/passwd`). Only import real files under the root.
        if (fs::is_symlink(entry, ec)) {
            continue;
        }
        if (!it->is_regular_file(ec) || entry.extension() != ".bru") {
            continue;
        }

        const auto fsize = fs::file_size(entry, ec);
        if (ec || fsize > kMaxFileBytes) {
            continue;
        }
        totalBytes += fsize;
        if (totalBytes > kMaxTotalBytes) {
            warnings.push_back(
                "collection exceeded the total size budget; some files were skipped");
            break;
        }

        // Path of this file relative to the collection root.
        const auto rel = fs::relative(entry, collectionRoot, ec);
        if (ec) {
            continue;
        }
        // Environments live under environments/.
        const auto firstComponent =
            rel.begin() != rel.end() ? rel.begin()->string() : std::string{};
        const auto contents = readFile(entry, ec);
        if (ec) {
            continue;
        }
        const auto blocks = parseBlocks(contents);

        if (firstComponent == "environments") {
            for (const auto& b : blocks) {
                if (b.name == "vars") {
                    for (const auto& [k, v] : parseDict(b.inner)) {
                        env[k] = rewrite(v);
                    }
                }
            }
            continue;
        }
        // Skip folder.bru meta files (they describe a folder, not a request).
        if (entry.filename() == "folder.bru") {
            continue;
        }

        // Find the method block; skip files without one (not a request).
        const BruBlock* methodBlock = nullptr;
        HttpMethod method = HttpMethod::Get;
        std::string reqName;
        std::string bodyType;
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> query;
        std::map<std::string, std::string> form;
        std::optional<std::string> rawBody;
        for (const auto& b : blocks) {
            if (const auto m = methodFromBlockName(b.name)) {
                methodBlock = &b;
                method = *m;
            } else if (b.name == "meta") {
                for (const auto& [k, v] : parseDict(b.inner)) {
                    if (k == "name") {
                        reqName = v;
                    }
                }
            } else if (b.name == "headers") {
                for (const auto& [k, v] : parseDict(b.inner)) {
                    headers[k] = rewrite(v);
                }
            } else if (b.name == "query" || b.name == "params") {
                for (const auto& [k, v] : parseDict(b.inner)) {
                    query[k] = rewrite(v);
                }
            } else if (b.name == "body") {
                const auto sub = toLowerAscii(b.subtype);
                if (sub == "json" || sub == "text" || sub == "xml" || sub == "sparql" ||
                    sub == "graphql" || sub.empty()) {
                    rawBody = rewrite(trim(b.inner));
                } else if (sub.find("form") != std::string::npos) {
                    for (const auto& [k, v] : parseDict(b.inner)) {
                        form[k] = rewrite(v);
                    }
                }
            }
        }
        if (methodBlock == nullptr) {
            continue;
        }

        // The method block is a dictionary carrying url / body / auth.
        std::string rawUrl;
        for (const auto& [k, v] : parseDict(methodBlock->inner)) {
            if (k == "url") {
                rawUrl = v;
            } else if (k == "body") {
                bodyType = v;
            }
        }

        const std::string resourceId =
            firstComponent.empty() || firstComponent == rel.filename().string()
                ? defaultResource
                : sanitizeId(firstComponent, defaultResource);
        auto& resource = outcome.project.resources[ResourceId{resourceId}];
        if (resource.id.value.empty()) {
            resource.id = ResourceId{resourceId};
        }

        const std::string displayName = reqName.empty() ? entry.stem().string() : reqName;
        std::string opName = sanitizeId(displayName, "request");
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
        op.headers = std::move(headers);
        op.queryParams = std::move(query);
        if (!form.empty()) {
            op.bodyForm = std::move(form);
        } else if (rawBody && !rawBody->empty()) {
            op.bodyTemplate = std::move(rawBody);
        }

        Provenance prov;
        prov.source = Provenance::Source::BrunoImport;
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        if (!reqName.empty()) {
            prov.evidence["bruno_name"] = reqName;
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(
            invalid("bruno import: no request .bru files found under the collection"));
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
