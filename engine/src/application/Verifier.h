// Verifier — evaluates AI-imported / non-hand-written extractions against
// a sample response and tags each with Verified / Null / NoMatch /
// NotEvaluated / NoSample.
//
// The AI importer calls this before writing any operation to disk. The
// rule: refuse to write any operation whose extractions are not all
// Verified. Extractions tagged with a soft state (NoSample, NotEvaluated)
// surface in the review UI as "needs your input"; NoMatch / Null surface
// as "the AI got this wrong".
//
// Pure computation — no I/O, no engine state. Lives in the application
// layer because it parses JSON (third-party dep), which the domain layer
// is not allowed to pull in.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>

#include <algorithm>
#include <cctype>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

/// One extraction's verification outcome.
enum class VerificationStatus {
    Verified,       ///< JSONPath resolved to a non-null scalar of plausible type
    Null,           ///< JSONPath structurally valid but produced null or empty
    NoMatch,        ///< JSONPath did not resolve at all
    NotEvaluated,   ///< source kind cannot be verified statically (XPath, Regex, Cookie)
    NoSample,       ///< no sample response was available for verification
};

struct VerifiedExtraction {
    std::string variableName;
    std::string sourcePath;
    VerificationStatus status{VerificationStatus::NotEvaluated};

    /// On `Verified`: a short snippet of the resolved value (truncated to
    /// ~80 chars). On other statuses: a short human-readable explanation.
    std::string detail;
};

/// Aggregate result for one operation. Used to decide whether the operation
/// can be written (Verified or NoSample for every extraction) or surfaced
/// as "needs your input" (Null / NoMatch for any extraction).
struct VerificationReport {
    std::vector<VerifiedExtraction> extractions;

    /// True when every extraction is `Verified`.
    [[nodiscard]] bool allVerified() const noexcept;

    /// True when every extraction is either `Verified` or in a soft state
    /// (`NoSample`, `NotEvaluated`).
    [[nodiscard]] bool noFailures() const noexcept;

    /// True when at least one extraction is `Null` or `NoMatch`.
    [[nodiscard]] bool hasFailures() const noexcept;
};

/// Case-insensitive comparator with heterogeneous lookup. Headers compare
/// per RFC 7230 §3.2 — `ETag` and `etag` are the same header.
struct CaseInsensitiveLess {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) {
                return std::tolower(x) < std::tolower(y);
            });
    }
};

/// Sample response context. The verifier prefers strong sources (real
/// responses) over weak ones (synthetic LLM samples). `verifiedAgainst`
/// flows through to the resulting Provenance.
struct SampleResponse {
    /// Raw JSON body of the sample. May be empty.
    std::string body;

    /// Header lookup. Case-insensitive on key per RFC 7230.
    std::map<std::string, std::string, CaseInsensitiveLess> headers;

    /// HTTP status code. 0 = unknown.
    int statusCode{0};

    /// Tag that flows into Provenance.verifiedAgainst on success.
    Provenance::VerifiedAgainst kind{Provenance::VerifiedAgainst::None};
};

class Verifier {
public:
    Verifier();
    ~Verifier();

    /// Verify every extraction on `op` against `sample`. Pure function.
    ///
    /// Returns `ChainApiError{SchemaInvalid, ...}` only when one of the
    /// extraction source paths is itself malformed. A path that simply
    /// doesn't match the sample is NOT an error — it surfaces as `NoMatch`.
    [[nodiscard]] std::expected<VerificationReport, ChainApiError>
    verify(const Operation& op, const SampleResponse& sample) const;

    /// Verify with an empty sample. Every extraction comes back tagged
    /// `NoSample`. Used when no sample of any kind is available.
    [[nodiscard]] VerificationReport
    verifyWithoutSample(const Operation& op) const noexcept;
};

}  // namespace chainapi::engine
