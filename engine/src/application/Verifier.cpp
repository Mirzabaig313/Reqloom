// Verifier — see Verifier.h for the contract.
//
// JSONPath walking here duplicates the small subset used by JsonPathEvaluator
// and PredicateEvaluator. The infrastructure copy is on the wrong layer for
// the application-layer verifier to depend on; the predicate copy is hidden
// in an anonymous namespace. All three cover the same narrow grammar:
//   $.field  /  $.nested.field  /  $.array[0].field  /  $.field[0]
// When extending, update all three.
#include "Verifier.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace chainapi::engine {

namespace {

using Json = nlohmann::json;

constexpr std::size_t kDetailExcerpt = 80;

std::string truncate(std::string_view s) {
    if (s.size() <= kDetailExcerpt) return std::string{s};
    std::string out{s.substr(0, kDetailExcerpt)};
    out += "...";
    return out;
}

/// Walk the JSONPath subset declared above. Returns nullptr on miss.
/// Distinguishes "miss" (nullptr) from "hit a null value" (non-null pointer
/// to a JSON `null`) — the verifier needs that distinction for NoMatch vs Null.
const Json* walk(const Json& root, std::string_view path) {
    if (!path.starts_with("$")) return nullptr;
    path.remove_prefix(1);

    const Json* cur = &root;
    while (!path.empty()) {
        if (path.front() == '.') {
            path.remove_prefix(1);
            std::size_t end = 0;
            while (end < path.size() && path[end] != '.' && path[end] != '[') {
                ++end;
            }
            if (end == 0) return nullptr;
            const std::string name{path.substr(0, end)};
            if (!cur->is_object()) return nullptr;
            const auto it = cur->find(name);
            if (it == cur->end()) return nullptr;
            cur = &(*it);
            path.remove_prefix(end);
        } else if (path.front() == '[') {
            path.remove_prefix(1);
            std::size_t end = 0;
            while (end < path.size() && path[end] != ']') ++end;
            if (end >= path.size()) return nullptr;
            const auto digits = path.substr(0, end);
            std::size_t idx = 0;
            const auto* first = digits.data();
            const auto* last = first + digits.size();
            const auto fc = std::from_chars(first, last, idx);
            if (fc.ec != std::errc{}) return nullptr;
            if (!cur->is_array() || idx >= cur->size()) return nullptr;
            cur = &((*cur)[idx]);
            path.remove_prefix(end + 1);
        } else {
            return nullptr;
        }
    }
    return cur;
}

VerifiedExtraction verifyJsonPath(const Extraction& ext, const Json& body) {
    VerifiedExtraction out;
    out.variableName = ext.variableName;
    out.sourcePath = ext.sourcePath;

    const Json* hit = walk(body, ext.sourcePath);
    if (hit == nullptr) {
        out.status = VerificationStatus::NoMatch;
        out.detail = "path did not resolve in sample response";
        return out;
    }
    if (hit->is_null()) {
        out.status = VerificationStatus::Null;
        out.detail = "path resolved to null";
        return out;
    }
    // Empty string / empty array / empty object are treated as Null —
    // they're not useful for variable substitution.
    if (hit->is_string() && hit->get<std::string>().empty()) {
        out.status = VerificationStatus::Null;
        out.detail = "path resolved to empty string";
        return out;
    }
    if ((hit->is_array() || hit->is_object()) && hit->empty()) {
        out.status = VerificationStatus::Null;
        out.detail = "path resolved to empty " + std::string{hit->is_array() ? "array" : "object"};
        return out;
    }

    out.status = VerificationStatus::Verified;
    out.detail = truncate(hit->dump());
    return out;
}

/// Header extraction paths follow the convention `$.headers.<Name>`.
/// The header map uses a case-insensitive comparator (RFC 7230 §3.2).
VerifiedExtraction verifyHeader(
    const Extraction& ext, const std::map<std::string, std::string, CaseInsensitiveLess>& headers) {
    VerifiedExtraction out;
    out.variableName = ext.variableName;
    out.sourcePath = ext.sourcePath;

    constexpr std::string_view kPrefix = "$.headers.";
    if (!ext.sourcePath.starts_with(kPrefix)) {
        out.status = VerificationStatus::NotEvaluated;
        out.detail = "non-standard header source path";
        return out;
    }
    const auto wanted = ext.sourcePath.substr(kPrefix.size());

    const auto it = headers.find(wanted);
    if (it == headers.end()) {
        out.status = VerificationStatus::NoMatch;
        out.detail = "header not present in sample response";
        return out;
    }
    if (it->second.empty()) {
        out.status = VerificationStatus::Null;
        out.detail = "header present but empty";
        return out;
    }
    out.status = VerificationStatus::Verified;
    out.detail = truncate(it->second);
    return out;
}

VerifiedExtraction verifyStatusCode(const Extraction& ext, int statusCode) {
    VerifiedExtraction out;
    out.variableName = ext.variableName;
    out.sourcePath = ext.sourcePath;
    if (statusCode <= 0) {
        out.status = VerificationStatus::NoSample;
        out.detail = "no status code provided in sample";
        return out;
    }
    out.status = VerificationStatus::Verified;
    out.detail = std::to_string(statusCode);
    return out;
}

VerifiedExtraction verifyNotEvaluable(const Extraction& ext, std::string_view reason) {
    VerifiedExtraction out;
    out.variableName = ext.variableName;
    out.sourcePath = ext.sourcePath;
    out.status = VerificationStatus::NotEvaluated;
    out.detail = std::string{reason};
    return out;
}

}  // namespace

// ─── Aggregate accessors ─────────────────────────────────────────────────────

bool VerificationReport::allVerified() const noexcept {
    return std::all_of(extractions.begin(), extractions.end(), [](const auto& v) {
        return v.status == VerificationStatus::Verified;
    });
}

bool VerificationReport::hasFailures() const noexcept {
    return std::any_of(extractions.begin(), extractions.end(), [](const auto& v) {
        return v.status == VerificationStatus::Null || v.status == VerificationStatus::NoMatch;
    });
}

bool VerificationReport::noFailures() const noexcept {
    return !hasFailures();
}

// ─── Public API ──────────────────────────────────────────────────────────────

Verifier::Verifier() = default;
Verifier::~Verifier() = default;

std::expected<VerificationReport, ChainApiError> Verifier::verify(
    const Operation& op, const SampleResponse& sample) const {
    VerificationReport report;
    report.extractions.reserve(op.extractions.size());

    // Parse the sample body once. Empty or non-JSON body falls back to an
    // empty object — JSONPath extractions come back as NoMatch.
    const bool sampleHasBody = !sample.body.empty();
    Json bodyDoc;
    if (sampleHasBody) {
        try {
            bodyDoc = Json::parse(sample.body);
        } catch (const Json::parse_error&) {
            // Body provided but not JSON — JSONPath extractions fail with NoMatch.
            bodyDoc = Json::object();
        }
    }

    for (const auto& ext : op.extractions) {
        switch (ext.source) {
            case Extraction::Source::JsonPath:
                if (!sampleHasBody) {
                    VerifiedExtraction out;
                    out.variableName = ext.variableName;
                    out.sourcePath = ext.sourcePath;
                    out.status = VerificationStatus::NoSample;
                    out.detail = "no sample body available";
                    report.extractions.push_back(std::move(out));
                } else {
                    report.extractions.push_back(verifyJsonPath(ext, bodyDoc));
                }
                break;

            case Extraction::Source::Header:
                if (sample.headers.empty()) {
                    VerifiedExtraction out;
                    out.variableName = ext.variableName;
                    out.sourcePath = ext.sourcePath;
                    out.status = VerificationStatus::NoSample;
                    out.detail = "no sample headers available";
                    report.extractions.push_back(std::move(out));
                } else {
                    report.extractions.push_back(verifyHeader(ext, sample.headers));
                }
                break;

            case Extraction::Source::StatusCode:
                report.extractions.push_back(verifyStatusCode(ext, sample.statusCode));
                break;

            case Extraction::Source::XPath:
                report.extractions.push_back(
                    verifyNotEvaluable(ext, "xpath extractions are not statically verifiable"));
                break;

            case Extraction::Source::Regex:
                report.extractions.push_back(
                    verifyNotEvaluable(ext, "regex extractions are not statically verifiable"));
                break;

            case Extraction::Source::Cookie:
                report.extractions.push_back(
                    verifyNotEvaluable(ext, "cookie extractions require a live response"));
                break;
        }
    }

    return report;
}

VerificationReport Verifier::verifyWithoutSample(const Operation& op) const noexcept {
    VerificationReport report;
    report.extractions.reserve(op.extractions.size());
    for (const auto& ext : op.extractions) {
        VerifiedExtraction out;
        out.variableName = ext.variableName;
        out.sourcePath = ext.sourcePath;
        out.status = VerificationStatus::NoSample;
        out.detail = "verification skipped (no sample of any kind)";
        report.extractions.push_back(std::move(out));
    }
    return report;
}

}  // namespace chainapi::engine
