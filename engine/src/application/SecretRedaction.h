// SecretRedaction — value-based redaction (SecretRegistry) of everything the engine hands out, and
// safe response-body excerpts for failure details. Two layers: value redaction (every registered
// secret becomes `kRedactedValueMarker`) and name redaction (HeaderMasking.h: secret-looking names
// become `kRedactedHeaderValue`). Body excerpts also get field redaction (sensitive-named JSON
// keys, `key=value` pairs, XML elements), since a failed login response can carry a token the
// engine never registered. Captured bodies on events are exempt: they are the person's own view
// (see `maskCapturedBody` for agent surfaces).
#pragma once

#include <reqloom/engine/Events.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/RunContext.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

class SecretRegistry;

/// Copy of `body` with values of sensitive-named fields replaced by the marker: JSON is walked
/// structurally (key order kept), other text gets a conservative `key=value` / `key: value` /
/// `<key>value</key>` scan. nullopt when the body is not text (binary, NUL bytes, invalid UTF-8).
[[nodiscard]] std::optional<std::string> redactSensitiveBodyFields(std::string_view body);

/// Failure-detail excerpt of `body`: field redaction, then value redaction against `registry`
/// (when non-null), then at most `maxBytes` bytes (clamped to `kMaxFailureExcerptBytes`, cut
/// on a UTF-8 boundary) followed by "… (truncated, <total> total)" when cut. nullopt for an
/// empty body, `maxBytes == 0`, or a body that cannot be made safe; callers then omit it.
[[nodiscard]] std::optional<std::string> safeBodyExcerpt(
    std::string_view body,
    const SecretRegistry* registry,
    std::size_t maxBytes = kDefaultFailureExcerptBytes);

/// Failure text built from the parts of a JSON `body` that `paths` select (`failure_message.show`):
/// `<path>: <value>` per path that matched, in order, joined by "; ". A scalar prints as its
/// text, an object or array as compact JSON. Masked like `safeBodyExcerpt` (a path naming a
/// sensitive field prints the marker) and capped the same way. nullopt when the body is not
/// JSON, nothing matched, or `maxBytes == 0`: the caller then uses `safeBodyExcerpt`.
[[nodiscard]] std::optional<std::string> selectedBodyExcerpt(
    std::string_view body,
    const std::vector<std::string>& paths,
    const SecretRegistry* registry,
    std::size_t maxBytes = kDefaultFailureExcerptBytes);

/// Value-redact every free-text field of `event` in place (URLs, header
/// values, details, extracted values, diagnostics). `ResponseReceived::body`
/// is left as it was: captured bodies reach the person and history unmasked.
void redactEvent(RunEvent& event, const SecretRegistry& registry);

/// Value-redact the free-text fields of a step result (detail, assertions).
void redactStepResult(StepResult& step, const SecretRegistry& registry);

}  // namespace reqloom::engine
