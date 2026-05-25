// Verifier — evaluates AI-imported / non-hand-written extractions
// against a sample response and tags each with ✅ / ⚠️ / ❌.
//
// The AI importer (PRD §10) calls this *before* writing any operation
// to disk. Per §10.3.1 the rule is: refuse to write any operation
// whose extractions are not all ✅ verified. Extractions tagged ⚠️
// (no sample available, or extraction source the verifier can't
// evaluate without a network) surface in the review UI as "needs
// your input"; ❌ tags surface as "the AI got this wrong".
//
// Pure computation — no I/O, no engine state. Lives in the application
// layer because it parses JSON (third-party dep), which the domain
// layer is not allowed to pull in.
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

/// One extraction's verification outcome. PRD §10.3.1.
enum class VerificationStatus {
    Verified,       ///< ✅ JSONPath resolved to a non-null scalar of plausible type
    Null,           ///< ⚠️ JSONPath structurally valid but produced null
    NoMatch,        ///< ❌ JSONPath did not resolve at all
    NotEvaluated,   ///< ⚠️ source kind cannot be verified statically (XPath, Regex, Cookie)
    NoSample,       ///< ⚠️ no sample response was available for verification
};

struct VerifiedExtraction {
    std::string variableName;       ///< mirrors `Extraction::variableName`
    std::string sourcePath;         ///< the path that was evaluated
    VerificationStatus status{VerificationStatus::NotEvaluated};

    /// On `Verified`: a short snippet of the resolved value (truncated
    /// to ~80 chars) for the review UI's evidence column. On `Null` /
    /// `NoMatch` / `NotEvaluated`: a short human-readable explanation.
    std::string detail;
};

/// Aggregate result for one operation. PRD §10.3.6 review flow uses
/// this to decide whether the operation can be written (`Verified` or
/// `NoSample` for every extraction) or surfaced as "needs your input"
/// (`Null` / `NoMatch` for any extraction).
struct VerificationReport {
    std::vector<VerifiedExtraction> extractions;

    /// True when every extraction is `Verified`. Maps to "OK to write".
    [[nodiscard]] bool allVerified() const noexcept;

    /// True when every extraction is either `Verified` or in a soft
    /// state (`NoSample`, `NotEvaluated`). Maps to "OK to write with a
    /// warning that some extractions could not be evaluated".
    [[nodiscard]] bool noFailures() const noexcept;

    /// True when at least one extraction is `Null` or `NoMatch`. Maps
    /// to "refuse to write — needs your input" per §10.3.1.
    [[nodiscard]] bool hasFailures() const noexcept;
};

/// Case-insensitive comparator with heterogeneous lookup. Headers
/// must compare per RFC 7230 §3.2 — `ETag` and `etag` are the same
/// header. The transparent `is_transparent` typedef enables `find()`
/// with `std::string_view` directly without an intermediate string.
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

/// Sample response context. PRD §10.3.1: the verifier prefers strong
/// sources (real responses) over weak ones (synthetic LLM samples).
/// `verifiedAgainst` is plumbed through to the resulting Provenance
/// so the run-time diagnostics know how much to trust the inference.
struct SampleResponse {
    /// Raw JSON body of the sample. May be empty (verifier surfaces
    /// every JsonPath extraction as `NoSample`).
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
    /// extraction source paths is itself malformed (e.g. unparseable
    /// JSONPath). A path that simply doesn't match the sample is NOT an
    /// error — it surfaces as `NoMatch` in the report.
    [[nodiscard]] std::expected<VerificationReport, ChainApiError>
    verify(const Operation& op, const SampleResponse& sample) const;

    /// Convenience: verify with an empty sample. Every JsonPath/Header/
    /// StatusCode extraction comes back tagged `NoSample`. The importer
    /// uses this when no sample of any kind is available, so the report
    /// still records the "no verification" fact in provenance.
    [[nodiscard]] VerificationReport
    verifyWithoutSample(const Operation& op) const noexcept;
};

}  // namespace chainapi::engine
