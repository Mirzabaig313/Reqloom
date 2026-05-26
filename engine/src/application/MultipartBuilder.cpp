// MultipartBuilder — see header.
//
// Two routing rules in order:
//   1. If any header is `Content-Type: multipart/form-data` (case-
//      insensitive), the request is multipart regardless of values.
//      This matches what users wrote in their YAML — they explicitly
//      asked for multipart, so we honour it.
//   2. Otherwise, if any resolved value starts with `@`, we route to
//      multipart. The `@` prefix is the curl convention for file
//      fields; users mixing file uploads with form fields don't have
//      to set the Content-Type header by hand.
//
// File path resolution is conservative: paths must exist, be regular
// files, and fit under a 50 MiB cap. We do NOT enforce project-root
// containment — the user's own YAML legitimately references files in
// `~/Documents`, `/tmp`, etc. The threat model for `body_form` is "the
// person editing the YAML is the operator", same as for shell scripts.
// If that ever needs tightening (third-party / team-shared YAMLs), the
// containment hook is one if-statement away.

#include "MultipartBuilder.h"

#include "../domain/Codecs.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace chainapi::engine {

namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxUploadBytes = 50ULL * 1024 * 1024;  // 50 MiB

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

[[nodiscard]] bool headerSaysMultipart(const std::map<std::string, std::string>& headers) {
    for (const auto& [k, v] : headers) {
        if (toLower(k) == "content-type" && toLower(v).starts_with("multipart/form-data")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool anyValueIsFileRef(const std::map<std::string, std::string>& resolvedFormFields) {
    return std::any_of(resolvedFormFields.begin(), resolvedFormFields.end(), [](const auto& kv) {
        return kv.second.starts_with("@");
    });
}

}  // namespace

bool wantsMultipart(const std::map<std::string, std::string>& headers,
                    const std::map<std::string, std::string>& resolvedFormFields) {
    return headerSaysMultipart(headers) || anyValueIsFileRef(resolvedFormFields);
}

std::expected<FormBody, ChainApiError> buildFormBody(
    const std::map<std::string, std::string>& resolvedFormFields, bool routeMultipart) {
    if (!routeMultipart) {
        std::string urlEncoded;
        for (const auto& [k, v] : resolvedFormFields) {
            if (!urlEncoded.empty()) urlEncoded += "&";
            urlEncoded += codecs::urlEncode(k) + "=" + codecs::urlEncode(v);
        }
        return FormBody{UrlEncodedBody{std::move(urlEncoded)}};
    }

    MultipartBody mb;
    mb.parts.reserve(resolvedFormFields.size());

    for (const auto& [k, v] : resolvedFormFields) {
        MultipartPart part;
        part.name = k;

        if (!v.starts_with("@")) {
            part.value = v;
            mb.parts.push_back(std::move(part));
            continue;
        }

        // File reference. Validate before queueing for upload — failing
        // here gives a clearer error than letting curl_mime fail later.
        const std::string rawPath = v.substr(1);  // drop leading '@'
        if (rawPath.empty()) {
            return std::unexpected(
                ChainApiError{ErrorCode::UploadFileUnreadable,
                              ErrorClass::Resolution,
                              "body_form field '" + k + "': empty file path after '@'"});
        }

        std::error_code ec;
        const auto canonical = fs::weakly_canonical(fs::path{rawPath}, ec);
        const auto& effective = ec ? fs::path{rawPath} : canonical;

        if (!fs::exists(effective, ec) || ec) {
            return std::unexpected(
                ChainApiError{ErrorCode::UploadFileUnreadable,
                              ErrorClass::Resolution,
                              "body_form field '" + k + "': file does not exist: " + rawPath});
        }
        if (!fs::is_regular_file(effective, ec) || ec) {
            return std::unexpected(
                ChainApiError{ErrorCode::UploadFileUnreadable,
                              ErrorClass::Resolution,
                              "body_form field '" + k + "': not a regular file: " + rawPath});
        }
        const auto size = fs::file_size(effective, ec);
        if (ec) {
            return std::unexpected(
                ChainApiError{ErrorCode::UploadFileUnreadable,
                              ErrorClass::Resolution,
                              "body_form field '" + k + "': could not stat: " + rawPath});
        }
        if (size > kMaxUploadBytes) {
            return std::unexpected(ChainApiError{ErrorCode::UploadFileUnreadable,
                                                 ErrorClass::Resolution,
                                                 "body_form field '" + k +
                                                     "': file exceeds 50 MiB upload cap (" +
                                                     std::to_string(size) + " bytes): " + rawPath});
        }

        part.filePath = effective.string();
        part.filename = effective.filename().string();
        mb.parts.push_back(std::move(part));
    }

    return FormBody{std::move(mb)};
}

}  // namespace chainapi::engine
