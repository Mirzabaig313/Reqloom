// Display-string helpers that map engine value types to UI text.
// Keeps enum→string switches in one place so panels stay consistent.
#pragma once

#include <chainapi/engine/Events.h>
#include <chainapi/engine/Operation.h>
#include <chainapi/engine/RunContext.h>

#include <QtCore/QString>

namespace chainapi::desktop::format {

/// Uppercase HTTP verb, e.g. "POST".
[[nodiscard]] QString method(engine::HttpMethod method);

/// Short status glyph for a step ("OK", "FAIL", "SKIP", …).
[[nodiscard]] QString statusGlyph(engine::StepResult::Status status);

/// Human label for a run outcome ("Succeeded", "Failed", "Cancelled").
[[nodiscard]] QString runOutcome(engine::RunOutcome outcome);

/// Stable error-code string (e.g. "E_CYCLE") for an error code.
[[nodiscard]] QString errorCode(engine::ErrorCode code);

/// Label for an extraction source kind ("jsonpath", "header", …).
[[nodiscard]] QString extractionSource(engine::Extraction::Source source);

/// Label for a streamed extraction outcome ("resolved", "null", "missing", …).
[[nodiscard]] QString extractionOutcome(engine::ExtractionCompleted::Outcome outcome);

/// Label for a skip reason ("session valid", "extraction cached").
[[nodiscard]] QString skipReason(engine::SkipReason reason);

}  // namespace chainapi::desktop::format
