// Project identity helpers — see reqloom/engine/ProjectIdentity.h.
#include <reqloom/engine/ProjectIdentity.h>

#include <reqloom/engine/ExecutionEngine.h>

#include <reqloom/engine/ProjectTrustStore.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace reqloom::engine {

std::expected<void, ReqloomError> validateProjectId(const std::string_view id) {
    const auto invalid = [](const std::string& why) {
        return std::unexpected(
            ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, "project id " + why});
    };
    if (id.empty()) {
        return invalid("must not be empty");
    }
    if (id.size() > kMaxProjectIdLength) {
        return invalid(std::format("must be at most {} characters", kMaxProjectIdLength));
    }
    for (const char c : id) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '/') {
            return invalid("must not contain '/'");
        }
        if (byte < 0x20 || byte == 0x7F) {
            return invalid("must not contain control characters");
        }
    }
    return {};
}

std::string generateProjectId() {
    // random_device is the OS entropy source on every supported platform.
    std::random_device rd;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(rd() & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);

    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out += std::format("{:02x}", bytes[i]);
    }
    return out;
}

bool windowsFolderPathsEqual(const std::string_view a, const std::string_view b) {
    const auto normalised = [](std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == '/') {
                c = '\\';
            } else if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            out.push_back(c);
        }
        // A drive root keeps its separator.
        while (out.size() > 1 && out.back() == '\\' && !(out.size() == 3 && out[1] == ':')) {
            out.pop_back();
        }
        return out;
    };
    return normalised(a) == normalised(b);
}

namespace {

namespace fs = std::filesystem;

/// The file system decides when both folders exist (letter case, 8.3 names, links); a missing
/// folder (a moved project) falls back to comparing normalised paths.
[[nodiscard]] bool sameFolder(const fs::path& recorded, const fs::path& current) {
    std::error_code ec;
    if (fs::exists(recorded, ec) && fs::exists(current, ec)) {
        const bool same = fs::equivalent(recorded, current, ec);
        return same && !ec;
    }
    const auto normal = [](const fs::path& p) {
        auto out = p.lexically_normal();
        if (!out.has_filename() && out.has_relative_path()) {
            out = out.parent_path();
        }
        return out;
    };
#ifdef _WIN32
    const auto text = [](const fs::path& p) {
        const auto u8 = p.u8string();
        return std::string{u8.begin(), u8.end()};
    };
    return windowsFolderPathsEqual(text(normal(recorded)), text(normal(current)));
#else
    return normal(recorded) == normal(current);
#endif
}

[[nodiscard]] std::expected<void, ReqloomError> requireProjectId(const Project& project) {
    if (project.id.empty()) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "this project uses secrets but has no id, so its secrets cannot be looked up. "
            "Add `id: " +
                generateProjectId() + "` to reqloom.yaml (any unique value without '/')."});
    }
    return validateProjectId(project.id);
}

[[nodiscard]] ReqloomError folderError(std::string detail) {
    return ReqloomError{ErrorCode::SecretAccessFailed, ErrorClass::Auth, std::move(detail)};
}

/// The project's folder with symlinks resolved, so a link and its target
/// count as the same folder.
[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalFolder(const Project& project) {
    if (project.rootDir.empty()) {
        return std::unexpected(folderError(
            "this project uses secrets but was not loaded from a folder, so its secrets "
            "cannot be checked against the folder that owns its id. Open the project from "
            "its folder (the one holding reqloom.yaml) to use secrets."));
    }
    std::error_code ec;
    auto folder = fs::weakly_canonical(project.rootDir, ec);
    if (ec) {
        return std::unexpected(folderError("cannot resolve the project folder '" +
                                           project.rootDir.string() + "': " + ec.message()));
    }
    if (!folder.has_filename() && folder.has_relative_path()) {
        folder = folder.parent_path();  // drop a trailing separator
    }
    return folder;
}

[[nodiscard]] ReqloomError conflictError(const Project& project,
                                         const fs::path& owner,
                                         const fs::path& here) {
    return folderError("secrets were not loaded: project id '" + project.id +
                       "' belongs to the folder '" + owner.string() +
                       "' on this machine, but this project is in '" + here.string() +
                       "'. If you moved the project, open it in the Reqloom desktop app and "
                       "confirm the move in Manage Secrets. If this is a copy, give the copy "
                       "its own id: change the `id:` line in its reqloom.yaml to `id: " +
                       generateProjectId() + "`.");
}

}  // namespace

std::expected<std::string, ReqloomError> projectSecretKey(const Project& project,
                                                          const std::string_view name) {
    if (auto ok = requireProjectId(project); !ok) {
        return std::unexpected(ok.error());
    }
    return project.id + "/" + std::string{name};
}

std::expected<void, ReqloomError> checkProjectFolder(ProjectTrustStore* trust,
                                                     const Project& project) {
    if (auto ok = requireProjectId(project); !ok) {
        return ok;
    }
    auto folder = canonicalFolder(project);
    if (!folder) {
        return std::unexpected(folder.error());
    }
    if (trust == nullptr) {
        return std::unexpected(
            folderError("no project trust store is configured, so this project's folder cannot be "
                        "checked; secrets were not loaded"));
    }
    auto owner = trust->recordIfAbsent(project.id, *folder);
    if (!owner) {
        return std::unexpected(owner.error());
    }
    if (!sameFolder(*owner, *folder)) {
        return std::unexpected(conflictError(project, *owner, *folder));
    }
    return {};
}

std::optional<std::filesystem::path> projectFolderConflict(ProjectTrustStore& trust,
                                                           const Project& project) {
    if (!requireProjectId(project)) {
        return std::nullopt;
    }
    auto folder = canonicalFolder(project);
    if (!folder) {
        return std::nullopt;
    }
    auto owner = trust.folderFor(project.id);
    if (!owner || !owner->has_value() || sameFolder(**owner, *folder)) {
        return std::nullopt;
    }
    return **owner;
}

std::expected<void, ReqloomError> rebindProjectFolder(ProjectTrustStore& trust,
                                                      const Project& project) {
    if (auto ok = requireProjectId(project); !ok) {
        return ok;
    }
    auto folder = canonicalFolder(project);
    if (!folder) {
        return std::unexpected(folder.error());
    }
    return trust.rebind(project.id, *folder);
}

namespace {

/// One line: [begin, end) excludes the line break; `next` starts the following line.
struct TextLine {
    std::size_t begin{0};
    std::size_t end{0};
    std::size_t next{0};
};

[[nodiscard]] std::vector<TextLine> splitLines(const std::string_view text, std::size_t pos) {
    std::vector<TextLine> lines;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.push_back({pos, text.size(), text.size()});
            break;
        }
        auto end = nl;
        if (end > pos && text[end - 1] == '\r') {
            --end;
        }
        lines.push_back({pos, end, nl + 1});
        pos = nl + 1;
    }
    return lines;
}

[[nodiscard]] bool isSpaceOrTab(const char c) {
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool isBlank(const std::string_view line) {
    return std::ranges::all_of(line, isSpaceOrTab);
}

/// `---` / `...` followed by end of line or whitespace.
[[nodiscard]] bool isMarker(const std::string_view line, const std::string_view marker) {
    return line.starts_with(marker) && (line.size() == 3 || isSpaceOrTab(line[3]));
}

[[nodiscard]] std::string_view trimmed(std::string_view s) {
    while (!s.empty() && isSpaceOrTab(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpaceOrTab(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// The key of a column-0 block-mapping line, or nullopt for anything else (flow collection,
/// sequence item, complex key, anchor, tag, continuation).
[[nodiscard]] std::optional<std::string_view> blockKey(const std::string_view line) {
    if (line.empty()) {
        return std::nullopt;
    }
    std::size_t afterKey = 0;
    std::string_view key;
    if (line[0] == '"' || line[0] == '\'') {
        const char quote = line[0];
        std::size_t i = 1;
        while (i < line.size() && line[i] != quote) {
            i += (quote == '"' && line[i] == '\\') ? 2 : 1;
        }
        if (i >= line.size()) {
            return std::nullopt;
        }
        key = line.substr(1, i - 1);
        afterKey = i + 1;
        while (afterKey < line.size() && isSpaceOrTab(line[afterKey])) {
            ++afterKey;
        }
        if (afterKey >= line.size() || line[afterKey] != ':') {
            return std::nullopt;
        }
    } else {
        constexpr std::string_view kIndicators = "-?:,[]{}#&*!|>%@`";
        if (kIndicators.find(line[0]) != std::string_view::npos || isSpaceOrTab(line[0])) {
            return std::nullopt;
        }
        afterKey = 0;
        while (afterKey < line.size() &&
               !(line[afterKey] == ':' &&
                 (afterKey + 1 == line.size() || isSpaceOrTab(line[afterKey + 1])))) {
            ++afterKey;
        }
        if (afterKey >= line.size()) {
            return std::nullopt;
        }
        key = trimmed(line.substr(0, afterKey));
    }
    if (afterKey + 1 < line.size() && !isSpaceOrTab(line[afterKey + 1])) {
        return std::nullopt;
    }
    return key;
}

/// Where the top-level keys are: the first, `name:` and whether `id:` exists. Error when the
/// root is not a block mapping that can be edited line by line.
struct RootLayout {
    std::size_t firstKey{std::string_view::npos};
    std::size_t name{std::string_view::npos};
    bool hasId{false};
};

[[nodiscard]] std::expected<RootLayout, std::string> scanRoot(const std::string_view text,
                                                              const std::vector<TextLine>& lines) {
    RootLayout layout;
    bool sawMarker = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = text.substr(lines[i].begin, lines[i].end - lines[i].begin);
        if (isBlank(line) || line.front() == '#') {
            continue;
        }
        const bool inPrologue = layout.firstKey == std::string_view::npos;
        if (isMarker(line, "---") || isMarker(line, "...")) {
            const auto rest = trimmed(line.substr(3));
            if (!inPrologue || sawMarker || line.front() == '.') {
                return std::unexpected("the file holds more than one YAML document");
            }
            if (!rest.empty() && rest.front() != '#') {
                return std::unexpected("the document marker line carries content");
            }
            sawMarker = true;
            continue;
        }
        if (inPrologue && line.front() == '%' && !sawMarker) {
            continue;  // %YAML / %TAG directive
        }
        if (isSpaceOrTab(line.front())) {
            if (inPrologue) {
                return std::unexpected("the top-level mapping is indented");
            }
            continue;  // part of the previous key's value
        }
        const auto key = blockKey(line);
        if (!key) {
            return std::unexpected(
                "the root is not a plain block mapping (flow style, a sequence, an anchor or "
                "tag, or a value spanning lines at column 0)");
        }
        if (inPrologue) {
            layout.firstKey = i;
        }
        if (*key == "id") {
            layout.hasId = true;
        } else if (*key == "name" && layout.name == std::string_view::npos) {
            layout.name = i;
        }
    }
    if (layout.firstKey == std::string_view::npos) {
        return std::unexpected("the file has no top-level key");
    }
    return layout;
}

/// The line index the id goes before: after `name:` and its value lines, else the first key.
[[nodiscard]] std::size_t idLineIndex(const std::string_view text,
                                      const std::vector<TextLine>& lines,
                                      const RootLayout& layout) {
    if (layout.name == std::string_view::npos) {
        return layout.firstKey;
    }
    std::size_t last = layout.name;
    for (std::size_t j = layout.name + 1; j < lines.size(); ++j) {
        const auto line = text.substr(lines[j].begin, lines[j].end - lines[j].begin);
        if (isBlank(line)) {
            continue;
        }
        if (!isSpaceOrTab(line.front())) {
            break;
        }
        last = j;
    }
    return last + 1;
}

[[nodiscard]] bool isPlainIdScalar(const std::string_view id) {
    return !id.empty() && std::ranges::all_of(id, [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

}  // namespace

std::expected<std::optional<std::string>, ReqloomError> insertProjectIdLine(
    const std::string_view yamlText, const std::string_view id) {
    const auto refuse = [](const std::string& why) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "cannot add `id:` to reqloom.yaml automatically: " + why + ". Add the line by hand."});
    };
    if (!validateProjectId(id) || !isPlainIdScalar(id)) {
        return refuse("the id is not a plain YAML value");
    }
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    const std::size_t bodyStart = yamlText.starts_with(kBom) ? kBom.size() : 0;
    const auto lines = splitLines(yamlText, bodyStart);
    const auto layout = scanRoot(yamlText, lines);
    if (!layout) {
        return refuse(layout.error());
    }
    if (layout->hasId) {
        return std::optional<std::string>{};
    }

    const std::string_view eol =
        yamlText.find("\r\n") != std::string_view::npos ? std::string_view{"\r\n"} : "\n";
    const std::string idLine = "id: " + std::string{id};
    const auto at = idLineIndex(yamlText, lines, *layout);

    std::string out{yamlText};
    if (at < lines.size()) {
        out.insert(lines[at].begin, idLine + std::string{eol});
    } else if (!out.empty() && out.back() == '\n') {
        out += idLine + std::string{eol};
    } else {
        out += std::string{eol} + idLine;
    }
    return std::optional<std::string>{std::move(out)};
}

}  // namespace reqloom::engine
