// StepEvidence — what the API returned, kept on each StepResult for agent-facing output
// (`RunOptions::recordResponses`, PRD FR-13.11a), and the masking it needs (FR-15.3, NFR-3.7).
//
// During the run a row holds the body and extracted values as received. `finalizeStepEvidence`
// runs once when the run ends: by then every credential the run sent is in the registry, so a
// token extracted under an ordinary name in step 1 and sent as `Authorization: Bearer …` in
// step 3 is masked in step 1's body and extraction too. Masking comes first, the cut last, so a
// token straddling the cut cannot leave a prefix behind.
#pragma once

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/RunContext.h>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

struct HttpRequest;
class SecretRegistry;

/// Most bytes of an extracted value shown after masking.
inline constexpr std::size_t kExtractedValueBytes = 256;

/// The `<resource>.<field>` references the project places where a credential goes: a
/// credential-named header, query parameter or body field of an operation, an inline or default
/// auth secret, an actor's auth config, sign-in and refresh requests, and credential-named inject
/// headers. An extraction that feeds one of them is a credential, whether or not the run gets as
/// far as sending it.
[[nodiscard]] std::set<std::string> credentialReferences(const Project& project);

/// Register every credential `req` carries: the value of each credential-named header (and the
/// part after an `Authorization` scheme word, the decoded `user:password` of Basic, each
/// `Cookie` pair's value), query parameter and JSON or form body field. A backstop for values a
/// hook or a computed template put there.
void registerSentCredentials(const HttpRequest& req, SecretRegistry& registry);

/// Bytes kept past a row's body limit, so masking sees the fields around the cut.
inline constexpr std::size_t kRetentionMarginBytes = std::size_t{64} * 1024;

/// Most raw body bytes one run keeps across all rows; rows after that keep none (truncated).
inline constexpr std::size_t kMaxRetainedBodyBytesPerRun = std::size_t{32} * 1024 * 1024;

/// Keep `body` on `row` as received, for the end-of-run pass: nothing when `limit` is 0, else at
/// most `limit + kRetentionMarginBytes` bytes (the total size is kept either way).
void keepResponseBody(StepResult& row, const std::string& body, std::size_t limit);

/// Register the value of every cookie a response sets (`Set-Cookie`), for agent-facing output.
void registerSetCookies(const std::vector<std::pair<std::string, std::string>>& headers,
                        SecretRegistry& registry);

/// Mask, then cut, every row's response and extracted values, and value-redact its detail and
/// assertions again with the registry as it stands at the end of the run.
void finalizeStepEvidence(std::vector<StepResult>& steps,
                          const RunContext& ctx,
                          std::size_t bodyLimit);

/// `step` without its response or extracted values, for the run context's own record.
[[nodiscard]] StepResult withoutEvidence(StepResult step);

}  // namespace reqloom::engine
