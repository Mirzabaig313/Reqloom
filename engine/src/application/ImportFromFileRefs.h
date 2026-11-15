// File-reference sanitising shared by the collection importers.
//
// In a Reqloom `body_form`, a value starting with `@` is a file upload
// (MultipartBuilder reads the file at run time). Imported collections come
// from someone else's machine, so an importer must never carry a directory
// path from the collection into an `@` value: a shared collection naming
// `/Users/x/.ssh/id_rsa` or `../../secret` would otherwise upload that file
// when run. Importers keep only the base name and warn.
//
// A relative `@name` is resolved by MultipartBuilder against the process's
// current working directory (it is not anchored to the project folder), so
// the warnings say exactly that.
#pragma once

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

/// Final component of `path`, accepting both `/` and `\` separators and a
/// leading drive letter (`C:a.png`), since collections come from any OS.
/// `.` and `..` are not file names and reduce to an empty name.
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
        "place `{}` in the directory Reqloom is run from (relative `@` paths resolve against "
        "the current working directory), or choose the file again in the form editor",
        name);
}

}  // namespace detail

/// `body_form` value for an imported FILE field whose source path is
/// `path`: `@<base name>` (after `rewrite`), or a bare `@` placeholder when
/// there is no usable name. Adds one warning to `warnings` when a directory
/// part was dropped; a path that was already a bare name warns nothing.
template <typename Rewrite>
[[nodiscard]] std::string fileFieldValue(std::string_view path,
                                         std::string_view opId,
                                         std::string_view field,
                                         std::vector<std::string>& warnings,
                                         Rewrite&& rewrite) {
    const auto base = fileBaseName(path);
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

/// `body_form` value for an imported TEXT field. Reqloom has no escape for
/// a literal leading `@`, so such a value would be sent as a file upload:
/// it is warned about and its path reduced to a base name exactly like a
/// file field, so a text field cannot smuggle in a directory path.
template <typename Rewrite>
[[nodiscard]] std::string textFieldValue(std::string_view value,
                                         std::string_view opId,
                                         std::string_view field,
                                         std::vector<std::string>& warnings,
                                         Rewrite&& rewrite) {
    if (!value.starts_with('@')) {
        return std::forward<Rewrite>(rewrite)(value);
    }
    const auto base = fileBaseName(value.substr(1));
    std::string detailText = base.hadDirectory
                                 ? std::format("its directory was dropped and only `{}` kept — {}",
                                               base.name,
                                               detail::placementHint(base.name))
                                 : std::string{"review the value"};
    if (base.hadDirectory && base.name.empty()) {
        detailText = "it had no file name and was imported as an empty file slot";
    }
    warnings.push_back(std::format(
        "operation {}: text field `{}` value `{}` starts with `@`, which Reqloom sends as a file "
        "upload (there is no escape for a literal leading `@`); {}",
        opId,
        field,
        value,
        detailText));
    if (base.name.empty()) {
        return "@";
    }
    return "@" + std::forward<Rewrite>(rewrite)(base.name);
}

}  // namespace reqloom::engine::import_files
