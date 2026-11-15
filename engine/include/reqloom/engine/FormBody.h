// Public, stateless form-body preview.
//
// Given an operation's `body_form` fields (and optionally its headers), reports
// how the engine would encode the request body at run time: as
// application/x-www-form-urlencoded, or as multipart/form-data when a field is
// a file reference (`@/path`) or the headers ask for multipart. Lets an editor
// show "this uploads 2 files / 3 fields" and surface a missing/oversized file
// before a real run — without exposing file bytes to the UI.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// How the engine would encode a `body_form`.
enum class FormBodyKind : std::uint8_t {
    UrlEncoded,  ///< application/x-www-form-urlencoded (`a=1&b=2`).
    Multipart,   ///< multipart/form-data (any file field or multipart header).
};

/// Metadata for one multipart part — never the file bytes.
struct FormPartPreview {
    std::string name;            ///< Field name.
    bool isFile{false};          ///< True when the value was an `@path` file reference.
    std::string filename;        ///< Resolved filename for a file part (empty for text).
    std::string resolvedPath;    ///< Canonical path for a file part (empty for text).
    std::uint64_t sizeBytes{0};  ///< File size, or the text value's byte length.
};

/// Preview of the encoded body. For `UrlEncoded`, `urlEncoded` holds the
/// `a=1&b=2` string (text only, safe to show) and `parts` is empty. For
/// `Multipart`, `parts` describes each part and `urlEncoded` is empty.
struct FormBodyPreview {
    FormBodyKind kind{FormBodyKind::UrlEncoded};
    std::string contentType;  ///< The Content-Type the engine would send.
    std::string urlEncoded;
    std::vector<FormPartPreview> parts;
    std::uint64_t totalBytes{0};  ///< Sum of part/body content bytes (excludes
                                  ///< multipart boundary/header framing).
};

/// Preview how `formFields` would be encoded. Each value is a `body_form` template as
/// written; `{{var}}` is not substituted. A field is a file upload only when its template
/// starts with a single `@`; `@@` sends text with one `@` removed. `headers` is consulted
/// only for a `Content-Type: multipart/form-data` opt-in.
///
/// A relative file path resolves against `projectRoot` (else the working directory), as a
/// run does. Returns `ReqloomError{UploadFileUnreadable}` for a missing file, a non-regular
/// file, or one over the 50 MiB upload cap.
///
/// Reads each referenced file once to validate it; intended for an on-demand
/// preview, not per-keystroke. Pure aside from those reads; never mutates.
[[nodiscard]] std::expected<FormBodyPreview, ReqloomError> previewFormBody(
    const std::map<std::string, std::string>& formFields,
    const std::map<std::string, std::string>& headers = {},
    const std::filesystem::path& projectRoot = {});

/// True when a `body_form` template is a file upload (one leading `@`, not `@@`).
[[nodiscard]] bool isFormFileTemplate(std::string_view tmpl) noexcept;

/// The `body_form` template that sends `text` literally: a leading `@` is
/// escaped as `@@`. Inverse of `formTemplateText`.
[[nodiscard]] std::string formTextTemplate(std::string_view text);

/// The text a non-file template sends (`@@mirza` -> `@mirza`). A file
/// template is returned unchanged.
[[nodiscard]] std::string formTemplateText(std::string_view tmpl);

}  // namespace reqloom::engine
