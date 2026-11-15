// MultipartBuilder — translate an Operation's `body_form` into either a
// list of multipart parts (when the op opts into multipart/form-data) or
// a URL-encoded application/x-www-form-urlencoded body. The branching
// logic lives in one place so the executor stays readable.

#pragma once

#include "../infrastructure/http/HttpClient.h"

#include <reqloom/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace reqloom::engine {

/// Result of translating a `body_form` map at request-build time.
///
/// `UrlEncoded` carries the canonical `a=1&b=2` string for the
/// x-www-form-urlencoded body. `Multipart` carries the parts list ready
/// for `HttpRequest::multipart`. The executor inspects which alternative
/// is present and sets the request body accordingly.
struct UrlEncodedBody {
    std::string body;
};
struct MultipartBody {
    std::vector<MultipartPart> parts;
};
using FormBody = std::variant<UrlEncodedBody, MultipartBody>;

/// Upload size caps for `@path` file parts, checked from the measured size before reading.
struct UploadLimits {
    /// Largest single file part (50 MiB).
    std::uintmax_t perPartBytes{50ULL * 1024 * 1024};
    /// Largest sum of all file parts in one request (256 MiB); text parts do not count.
    std::uintmax_t totalBytes{256ULL * 1024 * 1024};
};

enum class FormFieldKind : std::uint8_t {
    Text,
    File,
};

/// A `body_form` template classified from what is written in the YAML, before substitution:
///   - one leading `@` is a FILE field; `rest` is the path template after the `@`;
///   - a leading `@@` is TEXT with one `@` removed (`@@mirza` -> `@mirza`);
///   - anything else is TEXT; `rest` is the whole template.
/// The kind never depends on the substituted value, so a variable resolving to `@/etc/passwd`
/// is still sent as text.
struct FormFieldTemplate {
    FormFieldKind kind{FormFieldKind::Text};
    std::string_view rest;  ///< The template to resolve; views into the input.
};
[[nodiscard]] FormFieldTemplate classifyFormTemplate(std::string_view tmpl) noexcept;

/// One `body_form` field ready for encoding: its kind and resolved value (the text to send,
/// or the file path without the leading `@`).
struct FormField {
    FormFieldKind kind{FormFieldKind::Text};
    std::string value;
};
using FormFields = std::map<std::string, FormField>;

[[nodiscard]] FormFields literalFormFields(const std::map<std::string, std::string>& templates);

/// Whether the request goes out as multipart: a multipart/form-data Content-Type, or any
/// FILE field.
[[nodiscard]] bool wantsMultipart(const std::map<std::string, std::string>& headers,
                                  const FormFields& fields);

/// Build the request body from classified, resolved `body_form` fields.
///
///   - When `routeMultipart` is true, returns `MultipartBody`: TEXT fields
///     become text parts, FILE fields are read from disk.
///   - Otherwise returns `UrlEncodedBody` with `&`-joined `key=value`
///     pairs (URL-encoded). Callers route to multipart whenever a FILE
///     field is present (`wantsMultipart`).
///
/// A FILE field's path is used as written when absolute; a relative path resolves against
/// `projectRoot`, or the working directory when empty. `~` is not expanded.
///
/// Returns `UploadFileUnreadable` when a referenced file is missing,
/// not a regular file, exceeds the per-part cap, or would push the total
/// of all file parts past the request cap (`limits`).
[[nodiscard]] std::expected<FormBody, ReqloomError> buildFormBody(
    const FormFields& fields,
    bool routeMultipart,
    const std::filesystem::path& projectRoot,
    const UploadLimits& limits = UploadLimits{});

}  // namespace reqloom::engine
