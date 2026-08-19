// Display-string helpers that map engine value types to UI text.
// Keeps enum→string switches in one place so panels stay consistent.
#pragma once

#include "../theming/Theme.h"

#include <reqloom/engine/Events.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/RunContext.h>

#include <QtCore/QString>

namespace reqloom::desktop::format {

/// Uppercase HTTP verb, e.g. "POST".
[[nodiscard]] QString method(engine::HttpMethod method);

/// Status token a method verb is tinted with : GET/HEAD/OPTIONS
/// read as safe (success), POST as creation (running/cyan), DELETE as
/// destructive (error), PUT/PATCH as mutation (warning). Shared by the explorer
/// chips and the request address-bar pill so the vocabulary stays consistent.
[[nodiscard]] theming::StatusToken methodStatusToken(const QString& method);

/// HTTP method colour for a verb  — a dedicated mnemonic hue
/// per method, distinct from the status palette. GET blue, POST green, PUT
/// orange, PATCH yellow, DELETE red; HEAD/OPTIONS/unknown neutral. Shared by
/// the explorer chips, the address-bar pill, and the execution-chain view.
[[nodiscard]] theming::MethodColor methodColor(const QString& method);

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

}  // namespace reqloom::desktop::format
