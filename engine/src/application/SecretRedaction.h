// SecretRedaction — applies value-based redaction (SecretRegistry) to
// everything the engine hands out, and makes response-body excerpts safe to
// quote in failure details.
//
// Two layers protect a string before it leaves the engine:
//   1. Value redaction: every registered secret value is replaced by
//      `kRedactedValueMarker`, whatever header, variable or field it sits in.
//   2. Name redaction (HeaderMasking.h): header and extraction values under a
//      secret-looking name are replaced by `kRedactedHeaderValue`.
// Body excerpts additionally get field redaction: the values of
// sensitive-named JSON keys (at any depth), `key=value` / `key: value` pairs
// and XML elements are masked, because a failed login response can carry a
// token the engine never registered.
#pragma once

#include <reqloom/engine/Events.h>
#include <reqloom/engine/RunContext.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

class SecretRegistry;

/// Bytes of response body quoted in a failure detail.
inline constexpr std::size_t kBodyExcerptBytes = 200;

/// Copy of `body` with the values of sensitive-named fields replaced by the
/// marker. JSON bodies are walked structurally (key order kept); other text
/// gets a conservative `key=value` / `key: value` / `<key>value</key>` scan.
/// Returns nullopt when the body is not text (binary, NUL bytes, invalid
/// UTF-8) and so cannot be made safe.
[[nodiscard]] std::optional<std::string> redactSensitiveBodyFields(std::string_view body);

/// Failure-detail excerpt of `body`: field redaction, then value redaction
/// against `registry` (when non-null), then the first `kBodyExcerptBytes`
/// bytes (UTF-8 safe) plus "..." when cut. nullopt for an empty body or one
/// that cannot be made safe — callers then omit the excerpt.
[[nodiscard]] std::optional<std::string> safeBodyExcerpt(std::string_view body,
                                                         const SecretRegistry* registry);

/// Value-redact every free-text field of `event` in place (URLs, header
/// values, captured bodies, details, extracted values, diagnostics). A
/// captured body also gets field redaction, even with an empty registry.
void redactEvent(RunEvent& event, const SecretRegistry& registry);

/// Value-redact the free-text fields of a step result (detail, assertions).
void redactStepResult(StepResult& step, const SecretRegistry& registry);

}  // namespace reqloom::engine
