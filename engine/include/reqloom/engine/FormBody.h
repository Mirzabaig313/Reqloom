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
#include <map>
#include <string>
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
    std::uint64_t totalBytes{0};  ///< Sum of part/body sizes on the wire.
};

/// Preview how `formFields` (already `{{var}}`-substituted, or literal) would
/// be encoded. `headers` is consulted only for a `Content-Type:
/// multipart/form-data` opt-in. Returns `ReqloomError{UploadFileUnreadable}`
/// when an `@path` field names a missing file, a non-regular file, or one over
/// the 50 MiB upload cap — the same checks applied at run time.
///
/// Reads each referenced file once to validate it; intended for an on-demand
/// preview, not per-keystroke. Pure aside from those reads; never mutates.
[[nodiscard]] std::expected<FormBodyPreview, ReqloomError> previewFormBody(
    const std::map<std::string, std::string>& formFields,
    const std::map<std::string, std::string>& headers = {});

}  // namespace reqloom::engine
