// File-reference sanitising shared by the collection importers.
//
// In a `body_form`, a value starting with `@` is a file upload. Imported collections come from
// someone else's machine, so an importer must never carry a directory path into an `@` value
// (`/Users/x/.ssh/id_rsa` or `../../secret` would upload that file when run); importers keep
// only the base name and warn.
//
// A text value starting with `@` is escaped as `@@`, so it needs no path stripping. A relative
// `@name` resolves against the project folder.
#pragma once

#include <reqloom/engine/FormBody.h>

#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine::import_files {

/// Result of reducing an imported path to its final component.
struct FileBaseName {
    std::string name;          ///< base name; empty for an empty or directory-only path
    bool hadDirectory{false};  ///< true when anything was removed from `path`
};

/// Final component of `path`, accepting `/` and `\` separators and a drive letter; `.` and
/// `..` reduce to an empty name.
[[nodiscard]] inline FileBaseName fileBaseName(std::string_view path) {
    std::string_view rest = path;
    bool hadDirectory{false};
    if (const auto sep = rest.find_last_of("/\\"); sep != std::string_view::npos) {
        rest.remove_prefix(sep + 1);
        hadDirectory = true;
    } else if (rest.size() >= 2 && rest[1] == ':' &&
               std::isalpha(static_cast<unsigned char>(rest[0])) != 0) {
        rest.remove_prefix(2);
        hadDirectory = true;
    }
    if (rest == "." || rest == "..") {
        return FileBaseName{std::string{}, true};
    }
    return FileBaseName{std::string{rest}, hadDirectory};
}

namespace detail {

[[nodiscard]] inline std::string placementHint(std::string_view name) {
    return std::format(
        "place `{}` next to reqloom.yaml in the project folder (relative `@` paths resolve "
        "against the project folder), or choose the file again in the form editor",
        name);
}

/// A file name containing a `{{...}}` variable is only known at run time, when an imported
/// environment could put a full path back, so such a reference is never imported.
[[nodiscard]] inline bool namesAVariable(std::string_view name) {
    return name.find("{{") != std::string_view::npos;
}

inline void warnVariableFileName(std::string_view kind,
                                 std::string_view opId,
                                 std::string_view field,
                                 std::string_view original,
                                 std::vector<std::string>& warnings) {
    warnings.push_back(std::format(
        "operation {}: {} field `{}` value `{}` takes its file name from a variable, which "
        "could resolve to any path at run time; it was imported as an empty file slot — "
        "choose the file in the form editor",
        opId,
        kind,
        field,
        original));
}

}  // namespace detail

/// `body_form` value for an imported FILE field: `@<base name>` (after `rewrite`), or a bare
/// `@` placeholder when no usable name remains. Warns once when a directory part was dropped.
template <typename Rewrite>
[[nodiscard]] std::string fileFieldValue(std::string_view path,
                                         std::string_view opId,
                                         std::string_view field,
                                         std::vector<std::string>& warnings,
                                         Rewrite&& rewrite) {
    const auto base = fileBaseName(path);
    if (detail::namesAVariable(base.name)) {
        detail::warnVariableFileName("file", opId, field, path, warnings);
        return "@";
    }
    if (base.hadDirectory) {
        if (base.name.empty()) {
            warnings.push_back(std::format(
                "operation {}: file field `{}` pointed at `{}` on the exporting machine, which "
                "has no file name; it was imported as an empty file slot — choose the file in "
                "the form editor",
                opId,
                field,
                path));
        } else {
            warnings.push_back(std::format(
                "operation {}: file field `{}` pointed at `{}` on the exporting machine; the "
                "directory was dropped and only `{}` kept — {}",
                opId,
                field,
                path,
                base.name,
                detail::placementHint(base.name)));
        }
    }
    if (base.name.empty()) {
        return "@";
    }
    return "@" + std::forward<Rewrite>(rewrite)(base.name);
}

/// `body_form` value for an imported TEXT field: the value after `rewrite`, with a leading
/// `@` escaped as `@@` so it is not read as a file.
template <typename Rewrite>
[[nodiscard]] std::string textFieldValue(std::string_view value,
                                         std::string_view /*opId*/,
                                         std::string_view /*field*/,
                                         std::vector<std::string>& /*warnings*/,
                                         Rewrite&& rewrite) {
    return formTextTemplate(std::forward<Rewrite>(rewrite)(value));
}

}  // namespace reqloom::engine::import_files
