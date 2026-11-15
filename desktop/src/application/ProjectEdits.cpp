// ProjectEdits — see header. References must be read exactly as the engine's DependencyResolver
// scans them, or a rename would miss a reference that still creates a dependency.
#include "ProjectEdits.h"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <set>

namespace reqloom::desktop {

namespace {

using RefVisitor = std::function<void(std::string_view scope, std::string_view field)>;

/// A dotted reference's scope span [0, end), or nullopt when the text has no dot.
[[nodiscard]] std::optional<std::size_t> scopeEnd(std::string_view ref) {
    if (ref.find('.') == std::string_view::npos) {
        return std::nullopt;
    }
    return ref.find_first_of(".[");
}

[[nodiscard]] std::pair<std::size_t, std::size_t> trimmedSpan(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {s.size(), s.size()};
    }
    return {begin, s.find_last_not_of(" \t") + 1};
}

/// The [begin, end) span of the dotted reference inside one placeholder's inner text, or nullopt.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> referenceSpan(
    std::string_view inner) {
    const auto [begin, end] = trimmedSpan(inner);
    if (begin == end) {
        return std::nullopt;
    }
    const auto trimmed = inner.substr(begin, end - begin);
    if (trimmed.front() != '$') {
        return std::pair{begin, end};
    }
    const auto open = trimmed.find('(');
    if (open == std::string_view::npos || trimmed.back() != ')') {
        return std::nullopt;
    }
    const auto argRegion = trimmed.substr(open + 1, trimmed.size() - open - 2);
    const auto [argBegin, argEnd] = trimmedSpan(argRegion);
    if (argBegin == argEnd || argRegion[argBegin] == '"' || argRegion[argBegin] == '\'') {
        return std::nullopt;
    }
    const auto offset = begin + open + 1;
    return std::pair{offset + argBegin, offset + argEnd};
}

/// Call `onRef(offset in text, reference, scope length)` for every reference in `text`.
template <typename OnRef>
void walkPlaceholders(std::string_view text, OnRef&& onRef) {
    std::size_t pos = 0;
    while (true) {
        const auto open = text.find("{{", pos);
        if (open == std::string_view::npos) {
            return;
        }
        const auto innerBegin = open + 2;
        const auto close = text.find('}', innerBegin);
        if (close == std::string_view::npos) {
            return;
        }
        if (close == innerBegin || close + 1 >= text.size() || text[close + 1] != '}') {
            pos = open + 1;
            continue;
        }
        const auto inner = text.substr(innerBegin, close - innerBegin);
        if (const auto span = referenceSpan(inner)) {
            const auto ref = inner.substr(span->first, span->second - span->first);
            if (const auto end = scopeEnd(ref)) {
                onRef(innerBegin + span->first, ref, *end);
            }
        }
        pos = close + 2;
    }
}

void forEachReference(std::string_view text, const RefVisitor& visit) {
    walkPlaceholders(text, [&](std::size_t, std::string_view ref, std::size_t end) {
        const auto dot = ref.find('.');
        visit(ref.substr(0, end), ref.substr(dot + 1));
    });
}

using StringField = std::string engine::InlineAuth::*;

// The inline-auth fields the engine scans for references (DependencyResolver).
constexpr std::array<StringField, 27> kInlineAuthTemplates{
    &engine::InlineAuth::token,
    &engine::InlineAuth::username,
    &engine::InlineAuth::password,
    &engine::InlineAuth::apiKeyName,
    &engine::InlineAuth::apiKeyValue,
    &engine::InlineAuth::awsAccessKey,
    &engine::InlineAuth::awsSecretKey,
    &engine::InlineAuth::awsRegion,
    &engine::InlineAuth::awsService,
    &engine::InlineAuth::awsSessionToken,
    &engine::InlineAuth::oauthConsumerKey,
    &engine::InlineAuth::oauthConsumerSecret,
    &engine::InlineAuth::oauthToken,
    &engine::InlineAuth::oauthTokenSecret,
    &engine::InlineAuth::oauth2TokenUrl,
    &engine::InlineAuth::oauth2ClientId,
    &engine::InlineAuth::oauth2ClientSecret,
    &engine::InlineAuth::oauth2Scope,
    &engine::InlineAuth::oauth2AuthUrl,
    &engine::InlineAuth::oauth2CallbackUrl,
    &engine::InlineAuth::oauth2AccessToken,
    &engine::InlineAuth::jwtSecret,
    &engine::InlineAuth::jwtPayload,
    &engine::InlineAuth::mtlsCertPath,
    &engine::InlineAuth::mtlsKeyPath,
    &engine::InlineAuth::mtlsKeyPassword,
    &engine::InlineAuth::mtlsCaCertPath,
};

template <typename Auth, typename F>
void visitAuthTemplates(Auth& auth, F&& f) {
    for (const auto field : kInlineAuthTemplates) {
        f(auth.*field);
    }
}

template <typename Map, typename F>
void visitValues(Map& map, F&& f) {
    for (auto& entry : map) {
        f(entry.second);
    }
}

template <typename Op, typename F>
void visitOperationTemplates(Op& op, F&& f) {
    f(op.pathTemplate);
    if (op.bodyTemplate) {
        f(*op.bodyTemplate);
    }
    visitValues(op.headers, f);
    visitValues(op.queryParams, f);
    if (op.bodyForm) {
        visitValues(*op.bodyForm, f);
    }
    if (op.inlineAuth) {
        visitAuthTemplates(*op.inlineAuth, f);
    }
    if (op.pollUntil) {
        f(op.pollUntil->pathTemplate);
        f(op.pollUntil->successWhen);
        if (op.pollUntil->failWhen) {
            f(*op.pollUntil->failWhen);
        }
    }
    for (auto& assertion : op.assertions) {
        f(assertion.expr);
    }
}

template <typename Actor, typename F>
void visitActorTemplates(Actor& actor, F&& f) {
    visitValues(actor.authConfig, f);
    visitValues(actor.inject.headers, f);
    for (auto& step : actor.authSteps) {
        f(step.pathTemplate);
        if (step.bodyTemplate) {
            f(*step.bodyTemplate);
        }
        visitValues(step.headers, f);
    }
    if (actor.refresh) {
        f(actor.refresh->pathTemplate);
        if (actor.refresh->bodyTemplate) {
            f(*actor.refresh->bodyTemplate);
        }
        visitValues(actor.refresh->headers, f);
    }
}

/// Visit every template in the project with the label of its owner.
template <typename Project, typename F>
void visitProjectTemplates(Project& project, F&& f) {
    for (auto& [resId, resource] : project.resources) {
        for (auto& [opName, op] : resource.operations) {
            const auto owner = resId.value + "." + opName;
            visitOperationTemplates(op, [&](auto& text) { f(owner, text); });
        }
    }
    for (auto& [actorId, actor] : project.actors) {
        const auto owner = "actor " + actorId.value;
        visitActorTemplates(actor, [&](auto& text) { f(owner, text); });
    }
    for (auto& [envName, vars] : project.environments) {
        const auto owner = "environment " + envName;
        visitValues(vars, [&](auto& text) { f(owner, text); });
    }
    if (project.defaultAuth) {
        const std::string owner{"default auth"};
        visitAuthTemplates(*project.defaultAuth, [&](auto& text) { f(owner, text); });
    }
}

}  // namespace

bool isSafeFileStem(std::string_view name) {
    return !name.empty() && name.front() != '.' &&
           name.find_first_of(std::string_view{"/\\:\0", 4}) == std::string_view::npos;
}

bool isValidEntityName(std::string_view name) {
    return isSafeFileStem(name) && name.find('.') == std::string_view::npos;
}

QString invalidNameMessage() {
    return QStringLiteral("Name can't be empty or contain '.', '/', '\\' or ':'.");
}

std::optional<std::string> caseInsensitiveClash(const std::vector<std::string>& existing,
                                                std::string_view name,
                                                std::string_view self) {
    const QString wanted = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
    for (const auto& candidate : existing) {
        if (candidate == self) {
            continue;
        }
        if (QString::fromStdString(candidate).compare(wanted, Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::string renameReferenceScope(std::string_view text,
                                 std::string_view from,
                                 std::string_view to) {
    std::string out;
    out.reserve(text.size());
    std::size_t copied = 0;
    walkPlaceholders(text, [&](std::size_t at, std::string_view ref, std::size_t end) {
        if (ref.substr(0, end) != from) {
            return;
        }
        out.append(text.substr(copied, at - copied));
        out.append(to);
        copied = at + end;
    });
    out.append(text.substr(copied));
    return out;
}

void renameScopeInProject(engine::Project& project, std::string_view from, std::string_view to) {
    visitProjectTemplates(project, [&](const std::string&, std::string& text) {
        text = renameReferenceScope(text, from, to);
    });
}

std::vector<EntityReference> findScopeReferences(const engine::Project& project,
                                                 std::string_view scope,
                                                 std::optional<std::string_view> field) {
    std::vector<EntityReference> found;
    std::set<std::pair<std::string, std::string>> seen;
    visitProjectTemplates(project, [&](const std::string& owner, const std::string& text) {
        forEachReference(text, [&](std::string_view refScope, std::string_view refField) {
            if (refScope != scope || (field && refField != *field)) {
                return;
            }
            auto what = std::format("{{{{{}.{}}}}}", refScope, refField);
            if (seen.emplace(owner, what).second) {
                found.push_back({owner, std::move(what)});
            }
        });
    });
    return found;
}

QString describeReferences(const std::vector<EntityReference>& references) {
    QStringList parts;
    for (const auto& ref : references) {
        parts << QStringLiteral("%1 (%2)").arg(QString::fromStdString(ref.owner),
                                               QString::fromStdString(ref.what));
    }
    return parts.join(QStringLiteral(", "));
}

engine::Extraction::Source impliedExtractionSource(std::string_view path) {
    if (path.starts_with("$.headers.")) {
        return engine::Extraction::Source::Header;
    }
    if (path.starts_with("$.cookies.")) {
        return engine::Extraction::Source::Cookie;
    }
    if (path == "$.status_code") {
        return engine::Extraction::Source::StatusCode;
    }
    return engine::Extraction::Source::JsonPath;
}

std::vector<engine::Extraction> mergeExtractions(
    const std::vector<engine::Extraction>& existing,
    const std::vector<std::pair<QString, QString>>& rows) {
    std::vector<engine::Extraction> out;
    out.reserve(rows.size());
    for (const auto& [variable, path] : rows) {
        const QString name = variable.trimmed();
        if (name.isEmpty()) {
            continue;
        }
        engine::Extraction extraction;
        extraction.variableName = name.toStdString();
        extraction.sourcePath = path.trimmed().toStdString();
        extraction.source = impliedExtractionSource(extraction.sourcePath);
        const auto old =
            std::ranges::find(existing, extraction.variableName, &engine::Extraction::variableName);
        if (old != existing.end() && old->source != impliedExtractionSource(old->sourcePath)) {
            extraction.source = old->source;
        }
        out.push_back(std::move(extraction));
    }
    return out;
}

std::vector<engine::AuthStep> mergeAuthSteps(const std::vector<engine::AuthStep>& existing,
                                             const std::vector<AuthStepEdit>& edits) {
    std::set<std::string> claimed;
    for (const auto& edit : edits) {
        claimed.insert(edit.id);
    }
    std::vector<engine::AuthStep> out;
    out.reserve(edits.size());
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        engine::AuthStep step;
        if (const auto same = std::ranges::find(existing, edit.id, &engine::AuthStep::id);
            same != existing.end()) {
            step = *same;
        } else if (i < existing.size() && !claimed.contains(existing[i].id)) {
            step = existing[i];
        }
        const auto previous = step.extractions;
        step.id = edit.id;
        step.method = edit.method;
        step.pathTemplate = edit.pathTemplate;
        step.bodyTemplate = edit.bodyTemplate;
        step.expectStatus = edit.expectStatus;
        step.extractions = mergeExtractions(previous, edit.extractions);
        out.push_back(std::move(step));
    }
    return out;
}

QString chooseEnvironment(const QStringList& available,
                          const QString& saved,
                          const QString& defaultEnvironment) {
    if (!saved.isEmpty() && available.contains(saved)) {
        return saved;
    }
    if (!defaultEnvironment.isEmpty() && available.contains(defaultEnvironment)) {
        return defaultEnvironment;
    }
    return available.isEmpty() ? QString{} : available.front();
}

ProducerLookup producerFromPlan(
    const std::expected<engine::ResolvedPlan, engine::ReqloomError>& plan,
    const engine::Project& project,
    const engine::OperationId& target,
    std::string_view token) {
    ProducerLookup result;
    const QString shown = QString::fromUtf8(token.data(), static_cast<qsizetype>(token.size()));
    if (!plan) {
        result.problem = QStringLiteral("Can't tell which endpoint produces {{%1}}: %2")
                             .arg(shown, QString::fromStdString(plan.error().detail));
        return result;
    }
    const auto edge = std::ranges::find_if(plan->edges, [&](const engine::DependencyEdge& e) {
        return e.implicit && e.consumer == target && e.variable == token;
    });
    if (edge == plan->edges.end()) {
        result.problem =
            QStringLiteral("No endpoint in this request's chain produces {{%1}}.").arg(shown);
        return result;
    }
    const auto dot = edge->producer.value.find('.');
    const auto varDot = token.find('.');
    const auto resIt =
        project.resources.find(engine::ResourceId{edge->producer.value.substr(0, dot)});
    if (resIt != project.resources.end() && varDot != std::string_view::npos) {
        const auto opIt = resIt->second.operations.find(edge->producer.value.substr(dot + 1));
        if (opIt != resIt->second.operations.end()) {
            const auto variable = token.substr(varDot + 1);
            for (const auto& extraction : opIt->second.extractions) {
                if (extraction.variableName == variable) {
                    result.sourcePath = extraction.sourcePath;
                }
            }
        }
    }
    result.producer = edge->producer.value;
    return result;
}

}  // namespace reqloom::desktop
