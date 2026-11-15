// MultipartBuilder — translate an Operation's `body_form` into either a
// list of multipart parts (when the op opts into multipart/form-data) or
// a URL-encoded application/x-www-form-urlencoded body. The branching
// logic lives in one place so the executor stays readable.

#pragma once

#include "../infrastructure/http/HttpClient.h"

#include <reqloom/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <map>
#include <string>
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

/// Upload size caps applied while resolving `@path` file parts. Both are
/// checked from the file's measured size before its bytes are read.
struct UploadLimits {
    /// Largest single file part (50 MiB).
    std::uintmax_t perPartBytes{50ULL * 1024 * 1024};
    /// Largest sum of all file parts in one request (256 MiB); text parts
    /// do not count.
    std::uintmax_t totalBytes{256ULL * 1024 * 1024};
};

/// Whether the operation's headers route the request to multipart.
[[nodiscard]] bool wantsMultipart(const std::map<std::string, std::string>& headers,
                                  const std::map<std::string, std::string>& resolvedFormFields);

/// Build the request body from a `body_form` map whose values have
/// already been substituted by `VariableResolver`.
///
///   - When `routeMultipart` is true OR any resolved value starts with
///     `@`, returns `MultipartBody`. Values with leading `@` are treated
///     as filesystem paths (the `@` is stripped before filesystem checks).
///   - Otherwise returns `UrlEncodedBody` with `&`-joined `key=value`
///     pairs (URL-encoded).
///
/// The `@` prefix is the conventional curl / Postman / GiGwala syntax
/// for file fields. We honour it so existing schemas migrate cleanly.
///
/// Returns `UploadFileUnreadable` when a referenced file is missing,
/// not a regular file, exceeds the per-part cap, or would push the total
/// of all file parts past the request cap (`limits`).
[[nodiscard]] std::expected<FormBody, ReqloomError> buildFormBody(
    const std::map<std::string, std::string>& resolvedFormFields,
    bool routeMultipart,
    const UploadLimits& limits = UploadLimits{});

}  // namespace reqloom::engine
