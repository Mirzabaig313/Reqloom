// Formatting — see header. Enum→display-string maps for the UI panels.
#include "Formatting.h"

#include <QtCore/QVariantMap>

#include <algorithm>
#include <limits>

namespace reqloom::desktop::format {

namespace {

[[nodiscard]] QString useKindToken(engine::VariableUseKind kind) {
    switch (kind) {
        case engine::VariableUseKind::UrlPath:
            return QStringLiteral("urlPath");
        case engine::VariableUseKind::RawQuery:
            return QStringLiteral("rawQuery");
        case engine::VariableUseKind::Fragment:
            return QStringLiteral("fragment");
        case engine::VariableUseKind::NamedQuery:
            return QStringLiteral("namedQuery");
        case engine::VariableUseKind::Header:
            return QStringLiteral("header");
        case engine::VariableUseKind::Auth:
            return QStringLiteral("auth");
        case engine::VariableUseKind::Body:
            return QStringLiteral("body");
        case engine::VariableUseKind::FormField:
            return QStringLiteral("formField");
        case engine::VariableUseKind::Unknown:
            return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] QString locationText(const engine::UnresolvedVariableDiagnostic& diagnostic) {
    const QString name = QString::fromStdString(diagnostic.useName);
    switch (diagnostic.useKind) {
        case engine::VariableUseKind::UrlPath:
            return QStringLiteral("URL path");
        case engine::VariableUseKind::RawQuery:
            return QStringLiteral("Raw query");
        case engine::VariableUseKind::Fragment:
            return QStringLiteral("URL fragment");
        case engine::VariableUseKind::NamedQuery:
            return QStringLiteral("Query parameter “%1”").arg(name);
        case engine::VariableUseKind::Header:
            return QStringLiteral("Header “%1”").arg(name);
        case engine::VariableUseKind::Auth:
            return name.isEmpty() ? QStringLiteral("Authentication")
                                  : QStringLiteral("Authentication “%1”").arg(name);
        case engine::VariableUseKind::Body:
            return QStringLiteral("Request body");
        case engine::VariableUseKind::FormField:
            return QStringLiteral("Form field “%1”").arg(name);
        case engine::VariableUseKind::Unknown:
            return QStringLiteral("Request configuration");
    }
    return QStringLiteral("Request configuration");
}

[[nodiscard]] QString causeToken(engine::UnresolvedVariableCause cause) {
    switch (cause) {
        case engine::UnresolvedVariableCause::EnvironmentValueMissing:
            return QStringLiteral("environmentValueMissing");
        case engine::UnresolvedVariableCause::SecretValueMissing:
            return QStringLiteral("secretValueMissing");
        case engine::UnresolvedVariableCause::ActorSessionFieldMissing:
            return QStringLiteral("actorSessionFieldMissing");
        case engine::UnresolvedVariableCause::ResourceValueMissing:
            return QStringLiteral("resourceValueMissing");
        case engine::UnresolvedVariableCause::ExtractionMissing:
            return QStringLiteral("extractionMissing");
        case engine::UnresolvedVariableCause::ExtractionNull:
            return QStringLiteral("extractionNull");
        case engine::UnresolvedVariableCause::ExtractionInvalid:
            return QStringLiteral("extractionInvalid");
        case engine::UnresolvedVariableCause::ExtractionUnsupported:
            return QStringLiteral("extractionUnsupported");
        case engine::UnresolvedVariableCause::Unavailable:
            return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

[[nodiscard]] QString causeText(const engine::UnresolvedVariableDiagnostic& diagnostic) {
    const QString source = QString::fromStdString(diagnostic.sourceId);
    const QString field = QString::fromStdString(diagnostic.sourceField);
    switch (diagnostic.cause) {
        case engine::UnresolvedVariableCause::EnvironmentValueMissing:
            return QStringLiteral("Environment “%1” has no usable value for “%2”.")
                .arg(source, field);
        case engine::UnresolvedVariableCause::SecretValueMissing:
            return QStringLiteral("No usable value was available for secret “%1”.").arg(field);
        case engine::UnresolvedVariableCause::ActorSessionFieldMissing:
            return QStringLiteral("Actor “%1” did not provide session field “%2”.")
                .arg(source, field);
        case engine::UnresolvedVariableCause::ResourceValueMissing:
            return QStringLiteral("Resource “%1” has no usable value for “%2” in this run.")
                .arg(source, field);
        case engine::UnresolvedVariableCause::ExtractionMissing:
            return QStringLiteral(
                "The producer response did not contain the configured extraction.");
        case engine::UnresolvedVariableCause::ExtractionNull:
            return QStringLiteral("The configured extraction resolved to null.");
        case engine::UnresolvedVariableCause::ExtractionInvalid:
            return QStringLiteral("The configured extraction could not be evaluated.");
        case engine::UnresolvedVariableCause::ExtractionUnsupported:
            return QStringLiteral("The configured extraction source is unsupported.");
        case engine::UnresolvedVariableCause::Unavailable:
            return QStringLiteral(
                "No usable value was available in the current run when this request was prepared.");
    }
    return QStringLiteral(
        "No usable value was available in the current run when this request was prepared.");
}

[[nodiscard]] QString sourceKindToken(engine::VariableSourceKind kind) {
    switch (kind) {
        case engine::VariableSourceKind::Environment:
            return QStringLiteral("environment");
        case engine::VariableSourceKind::Secret:
            return QStringLiteral("secret");
        case engine::VariableSourceKind::Actor:
            return QStringLiteral("actor");
        case engine::VariableSourceKind::Resource:
            return QStringLiteral("resource");
        case engine::VariableSourceKind::Extraction:
            return QStringLiteral("extraction");
        case engine::VariableSourceKind::Unknown:
            return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

}  // namespace

QString method(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

theming::StatusToken methodStatusToken(const QString& method) {
    if (method == QStringLiteral("GET") || method == QStringLiteral("HEAD") ||
        method == QStringLiteral("OPTIONS")) {
        return theming::StatusToken::Success;
    }
    if (method == QStringLiteral("POST")) {
        return theming::StatusToken::Running;
    }
    if (method == QStringLiteral("DELETE")) {
        return theming::StatusToken::Error;
    }
    // PUT / PATCH — mutation.
    return theming::StatusToken::Warning;
}

theming::MethodColor methodColor(const QString& method) {
    if (method == QStringLiteral("GET")) {
        return theming::MethodColor::Get;
    }
    if (method == QStringLiteral("POST")) {
        return theming::MethodColor::Post;
    }
    if (method == QStringLiteral("PUT")) {
        return theming::MethodColor::Put;
    }
    if (method == QStringLiteral("PATCH")) {
        return theming::MethodColor::Patch;
    }
    if (method == QStringLiteral("DELETE")) {
        return theming::MethodColor::Delete;
    }
    // HEAD / OPTIONS / unknown — no dedicated hue.
    return theming::MethodColor::Neutral;
}

QString statusGlyph(engine::StepResult::Status status) {
    switch (status) {
        case engine::StepResult::Status::Pending:
            return QStringLiteral("…");
        case engine::StepResult::Status::Ready:
            return QStringLiteral("READY");
        case engine::StepResult::Status::Skipped:
            return QStringLiteral("SKIP");
        case engine::StepResult::Status::Succeeded:
            return QStringLiteral("OK");
        case engine::StepResult::Status::Failed:
            return QStringLiteral("FAIL");
        case engine::StepResult::Status::Cancelled:
            return QStringLiteral("CANC");
        case engine::StepResult::Status::Blocked:
            return QStringLiteral("BLOCK");
    }
    return QStringLiteral("?");
}

QString runOutcome(engine::RunOutcome outcome) {
    switch (outcome) {
        case engine::RunOutcome::Succeeded:
            return QStringLiteral("Succeeded");
        case engine::RunOutcome::Failed:
            return QStringLiteral("Failed");
        case engine::RunOutcome::Cancelled:
            return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString errorCode(engine::ErrorCode code) {
    const auto codeStr = engine::toCodeString(code);
    return QString::fromUtf8(codeStr.data(), static_cast<qsizetype>(codeStr.size()));
}

QString extractionSource(engine::Extraction::Source source) {
    switch (source) {
        case engine::Extraction::Source::JsonPath:
            return QStringLiteral("jsonpath");
        case engine::Extraction::Source::XPath:
            return QStringLiteral("xpath");
        case engine::Extraction::Source::Header:
            return QStringLiteral("header");
        case engine::Extraction::Source::StatusCode:
            return QStringLiteral("status");
        case engine::Extraction::Source::Regex:
            return QStringLiteral("regex");
        case engine::Extraction::Source::Cookie:
            return QStringLiteral("cookie");
    }
    return QStringLiteral("jsonpath");
}

QString extractionOutcome(engine::ExtractionCompleted::Outcome outcome) {
    switch (outcome) {
        case engine::ExtractionCompleted::Outcome::Resolved:
            return QStringLiteral("resolved");
        case engine::ExtractionCompleted::Outcome::Null:
            return QStringLiteral("null");
        case engine::ExtractionCompleted::Outcome::Missing:
            return QStringLiteral("missing");
        case engine::ExtractionCompleted::Outcome::InvalidPattern:
            return QStringLiteral("invalid pattern");
        case engine::ExtractionCompleted::Outcome::Unsupported:
            return QStringLiteral("unsupported");
    }
    return QStringLiteral("missing");
}

QString skipReason(engine::SkipReason reason) {
    switch (reason) {
        case engine::SkipReason::SessionValid:
            return QStringLiteral("session valid");
        case engine::SkipReason::ExtractionCached:
            return QStringLiteral("extraction cached");
    }
    return QStringLiteral("cached");
}

int boundedIndex(const std::size_t index) noexcept {
    // TimelineModel converts zero-based engine indexes to one-based rows.
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max() - 1);
    return static_cast<int>(std::min(index, kMax));
}

QVariantList unresolvedDiagnostics(
    const std::span<const engine::UnresolvedVariableDiagnostic> diagnostics) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(diagnostics.size()));
    for (const auto& diagnostic : diagnostics) {
        QVariantMap item;
        item.insert(QStringLiteral("token"),
                    QStringLiteral("{{%1}}").arg(QString::fromStdString(diagnostic.token)));
        item.insert(QStringLiteral("useKind"), useKindToken(diagnostic.useKind));
        item.insert(QStringLiteral("useName"), QString::fromStdString(diagnostic.useName));
        item.insert(QStringLiteral("location"), locationText(diagnostic));
        item.insert(QStringLiteral("cause"), causeToken(diagnostic.cause));
        item.insert(QStringLiteral("causeText"), causeText(diagnostic));
        item.insert(QStringLiteral("sourceKind"), sourceKindToken(diagnostic.sourceKind));
        item.insert(QStringLiteral("sourceId"), QString::fromStdString(diagnostic.sourceId));
        item.insert(QStringLiteral("sourceField"), QString::fromStdString(diagnostic.sourceField));
        item.insert(QStringLiteral("producerOperationId"),
                    diagnostic.producerOp ? QString::fromStdString(diagnostic.producerOp->value)
                                          : QString{});
        // A persisted index of SIZE_MAX would wrap to step 0 and read as
        // "no producer" while claiming one, so it is rejected rather than shown.
        qulonglong producerStep{};
        constexpr auto kMaxProducerIndex =
            static_cast<std::size_t>(std::numeric_limits<int>::max() - 1);
        if (diagnostic.producerStepIndex && *diagnostic.producerStepIndex <= kMaxProducerIndex) {
            producerStep = static_cast<qulonglong>(boundedIndex(*diagnostic.producerStepIndex) + 1);
        }
        item.insert(QStringLiteral("producerStep"), producerStep);
        result.append(item);
    }
    return result;
}

}  // namespace reqloom::desktop::format
