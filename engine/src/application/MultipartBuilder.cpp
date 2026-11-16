// MultipartBuilder — see header.
// Routing: multipart when a header is `Content-Type: multipart/form-data` (the user asked for it),
// or when any field is a FILE field. A field is a file when its TEMPLATE (before `{{var}}`
// substitution) starts with a single
// `@`; `@@` escapes a literal `@`, and a substituted value never turns text into a file.
// A relative path resolves against the project folder (the working directory for an in-memory
// project). Files must exist, be regular, and fit the per-part and per-request caps
// (`UploadLimits`). Project-root containment is deliberately NOT enforced: the YAML author is the
// operator, and files in
// ~/Documents or /tmp are legitimate.

#include "MultipartBuilder.h"

#include "../domain/Codecs.h"

#include <reqloom/engine/FormBody.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace reqloom::engine {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMiB = 1024ULL * 1024;

[[nodiscard]] std::string formatLimit(std::uintmax_t bytes) {
    if (bytes != 0 && bytes % kMiB == 0) {
        return std::format("{} MiB", bytes / kMiB);
    }
    return std::format("{} bytes", bytes);
}

/// Per-part and request-total size gates, applied to a file's measured size before its bytes
/// are read; `filesSoFar` sums the file parts already accepted.
[[nodiscard]] std::optional<ReqloomError> checkUploadSize(const std::string& field,
                                                          const std::string& rawPath,
                                                          std::uintmax_t size,
                                                          std::uintmax_t filesSoFar,
                                                          const UploadLimits& limits) {
    if (size > limits.perPartBytes) {
        return ReqloomError{ErrorCode::UploadFileUnreadable,
                            ErrorClass::Resolution,
                            std::format("body_form field '{}': file exceeds {} upload cap "
                                        "({} bytes): {}",
                                        field,
                                        formatLimit(limits.perPartBytes),
                                        size,
                                        rawPath)};
    }
    if (size > limits.totalBytes - std::min(filesSoFar, limits.totalBytes)) {
        return ReqloomError{
            ErrorCode::UploadFileUnreadable,
            ErrorClass::Resolution,
            std::format("body_form field '{}': total size of file uploads in this request "
                        "would be {} bytes, over the {} request cap: {}",
                        field,
                        filesSoFar + size,
                        formatLimit(limits.totalBytes),
                        rawPath)};
    }
    return std::nullopt;
}

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool headerSaysMultipart(const std::map<std::string, std::string>& headers) {
    return std::ranges::any_of(headers, [](const auto& kv) {
        return toLower(kv.first) == "content-type" &&
               toLower(kv.second).starts_with("multipart/form-data");
    });
}

[[nodiscard]] bool anyFileField(const FormFields& fields) {
    return std::ranges::any_of(
        fields, [](const auto& kv) { return kv.second.kind == FormFieldKind::File; });
}

/// Where a file field's path is looked up: as written when absolute, else under the project
/// root (or the working directory when unknown). `~` is not expanded.
[[nodiscard]] fs::path uploadPath(const std::string& rawPath, const fs::path& projectRoot) {
    fs::path p{rawPath};
    if (p.is_relative() && !projectRoot.empty()) {
        p = projectRoot / p;
    }
    return p;
}

}  // namespace

FormFieldTemplate classifyFormTemplate(std::string_view tmpl) noexcept {
    if (tmpl.starts_with("@@")) {
        return FormFieldTemplate{FormFieldKind::Text, tmpl.substr(1)};
    }
    if (tmpl.starts_with('@')) {
        return FormFieldTemplate{FormFieldKind::File, tmpl.substr(1)};
    }
    return FormFieldTemplate{FormFieldKind::Text, tmpl};
}

bool isFormFileTemplate(std::string_view tmpl) noexcept {
    return classifyFormTemplate(tmpl).kind == FormFieldKind::File;
}

std::string formTextTemplate(std::string_view text) {
    return text.starts_with('@') ? "@" + std::string{text} : std::string{text};
}

std::string formTemplateText(std::string_view tmpl) {
    const auto t = classifyFormTemplate(tmpl);
    return t.kind == FormFieldKind::File ? std::string{tmpl} : std::string{t.rest};
}

FormFields literalFormFields(const std::map<std::string, std::string>& templates) {
    FormFields out;
    for (const auto& [k, v] : templates) {
        const auto t = classifyFormTemplate(v);
        out.emplace(k, FormField{t.kind, std::string{t.rest}});
    }
    return out;
}

bool wantsMultipart(const std::map<std::string, std::string>& headers, const FormFields& fields) {
    return headerSaysMultipart(headers) || anyFileField(fields);
}

std::expected<FormBody, ReqloomError> buildFormBody(const FormFields& fields,
                                                    bool routeMultipart,
                                                    const fs::path& projectRoot,
                                                    const UploadLimits& limits) {
    if (!routeMultipart) {
        std::string urlEncoded;
        for (const auto& [k, field] : fields) {
            if (!urlEncoded.empty()) {
                urlEncoded += "&";
            }
            urlEncoded += codecs::urlEncode(k) + "=" + codecs::urlEncode(field.value);
        }
        return FormBody{UrlEncodedBody{std::move(urlEncoded)}};
    }

    MultipartBody mb;
    mb.parts.reserve(fields.size());
    std::uintmax_t fileBytes{0};  // sum of accepted file parts, for the total cap

    for (const auto& [k, field] : fields) {
        MultipartPart part;
        part.name = k;

        if (field.kind == FormFieldKind::Text) {
            part.value = field.value;
            mb.parts.push_back(std::move(part));
            continue;
        }

        // File reference: the bytes load through an open fd so size and content can't be swapped
        // between stat and read.
        if (field.value.empty()) {
            return std::unexpected(
                ReqloomError{ErrorCode::UploadFileUnreadable,
                             ErrorClass::Resolution,
                             "body_form field '" + k + "': empty file path after '@'"});
        }

        const auto lookup = uploadPath(field.value, projectRoot);
        const std::string rawPath = lookup.string();  // the path looked up, for messages
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(lookup, ec);
        const auto& effective = ec ? lookup : canonical;

        // Cosmetic gates for friendly errors on a typoed path or a directory; racy on their own, so
        // the real checks go through an open fd.
        if (!fs::exists(effective, ec) || ec) {
            return std::unexpected(ReqloomError{
                ErrorCode::UploadFileUnreadable,
                ErrorClass::Resolution,
                std::format("body_form field '{}': file does not exist: {}", k, rawPath)});
        }
        if (!fs::is_regular_file(effective, ec) || ec) {
            return std::unexpected(ReqloomError{
                ErrorCode::UploadFileUnreadable,
                ErrorClass::Resolution,
                std::format("body_form field '{}': not a regular file: {}", k, rawPath)});
        }

        // From here on the path is irrelevant: tellg()/read() use the inode we already hold, even
        // if the path is swapped for a symlink; the caps below bound RSS.
        std::ifstream in(effective, std::ios::binary);
        if (!in) {
            return std::unexpected(
                ReqloomError{ErrorCode::UploadFileUnreadable,
                             ErrorClass::Resolution,
                             std::format("body_form field '{}': could not open: {}", k, rawPath)});
        }

        in.seekg(0, std::ios::end);
        const auto endPos = in.tellg();
        if (endPos == std::streampos{-1}) {
            return std::unexpected(ReqloomError{
                ErrorCode::UploadFileUnreadable,
                ErrorClass::Resolution,
                std::format("body_form field '{}': could not measure size: {}", k, rawPath)});
        }
        const auto size = static_cast<std::uintmax_t>(endPos);
        if (auto sizeErr = checkUploadSize(k, rawPath, size, fileBytes, limits)) {
            return std::unexpected(std::move(*sizeErr));
        }
        fileBytes += size;
        in.seekg(0, std::ios::beg);

        // Streambuf-iterator reads every byte verbatim, including NULs.
        std::string contents;
        contents.reserve(static_cast<std::size_t>(size));
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
        if (in.bad()) {
            return std::unexpected(
                ReqloomError{ErrorCode::UploadFileUnreadable,
                             ErrorClass::Resolution,
                             std::format("body_form field '{}': read error: {}", k, rawPath)});
        }

        part.value = std::move(contents);
        part.filePath = effective.string();
        part.filename = effective.filename().string();
        mb.parts.push_back(std::move(part));
    }

    return FormBody{std::move(mb)};
}

std::expected<FormBodyPreview, ReqloomError> previewFormBody(
    const std::map<std::string, std::string>& formFields,
    const std::map<std::string, std::string>& headers,
    const fs::path& projectRoot) {
    const auto fields = literalFormFields(formFields);
    const bool route = wantsMultipart(headers, fields);
    auto built = buildFormBody(fields, route, projectRoot);
    if (!built) {
        return std::unexpected(built.error());
    }

    FormBodyPreview preview;
    if (const auto* url = std::get_if<UrlEncodedBody>(&*built)) {
        preview.kind = FormBodyKind::UrlEncoded;
        preview.contentType = "application/x-www-form-urlencoded";
        preview.urlEncoded = url->body;
        preview.totalBytes = url->body.size();
        return preview;
    }

    const auto& mb = std::get<MultipartBody>(*built);
    preview.kind = FormBodyKind::Multipart;
    preview.contentType = "multipart/form-data";
    preview.parts.reserve(mb.parts.size());
    for (const auto& part : mb.parts) {
        FormPartPreview pp;
        pp.name = part.name;
        pp.isFile = part.isFile();
        // `part.value` holds file bytes or text; only its length is exposed.
        pp.sizeBytes = part.value.size();
        if (part.filename) {
            pp.filename = *part.filename;
        }
        if (part.filePath) {
            pp.resolvedPath = *part.filePath;
        }
        preview.totalBytes += pp.sizeBytes;
        preview.parts.push_back(std::move(pp));
    }
    return preview;
}

}  // namespace reqloom::engine
