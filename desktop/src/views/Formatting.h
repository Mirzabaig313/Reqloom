// Display-string helpers mapping engine value types to UI text, keeping enum→string switches in one
// place.
#pragma once

#include "../theming/Theme.h"

#include <reqloom/engine/Events.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/RunContext.h>

#include <QtCore/QString>
#include <QtCore/QVariantList>

#include <cstddef>
#include <span>

namespace reqloom::desktop::format {

/// Uppercase HTTP verb, e.g. "POST".
[[nodiscard]] QString method(engine::HttpMethod method);

/// Status token a method verb is tinted with: GET/HEAD/OPTIONS success, POST running, DELETE error,
/// PUT/PATCH warning.
[[nodiscard]] theming::StatusToken methodStatusToken(const QString& method);

/// HTTP method colour for a verb: GET blue, POST green, PUT orange, PATCH yellow, DELETE red;
/// others neutral.
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

/// Narrow an engine index to the timeline's `int` contract. Clamps rather than casts, so a corrupt
/// history index can't wrap to a negative row aliasing a real step.
[[nodiscard]] int boundedIndex(std::size_t index) noexcept;

/// Convert unresolved-variable evidence to ordered, value-free QML maps (source names and fields
/// only, never resolved or secret values). Producer indexes become one-based step numbers.
[[nodiscard]] QVariantList unresolvedDiagnostics(
    std::span<const engine::UnresolvedVariableDiagnostic> diagnostics);

}  // namespace reqloom::desktop::format
