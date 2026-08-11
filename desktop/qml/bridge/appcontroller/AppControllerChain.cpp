// AppController — variables, extraction, and chain-editor methods.
// Split from AppController.cpp; see AppController.h
#include "AppController.h"

#include "ThemeController.h"
#include "application/EnvironmentSettings.h"
#include "application/ExecutionPreview.h"
#include "application/ProjectModel.h"
#include "application/WorkspaceModel.h"
#include "views/Formatting.h"
#include "views/HookEditorDialog.h"
#include "views/PathEval.h"
#include "widgets/GraphLayout.h"
#include "widgets/LineDiff.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/Predicate.h>

#include <QtConcurrent/QtConcurrentRun>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include "AppControllerInternal.h"

namespace reqloom::desktop::qml {

QStringList AppController::editDependencyCandidates() const {
    // Every operation except the open one (no self-dependency).
    const QString self = currentOperationId();
    QStringList out;
    for (const QString& id : operationIds()) {
        if (id != self) {
            out.append(id);
        }
    }
    return out;
}

QStringList AppController::extractedVariablesFor(const QString& operationId) const {
    QStringList tokens;
    if (!activeProject().hasProject()) {
        return tokens;
    }
    const auto* op = activeProject().findOperation(engine::OperationId{operationId.toStdString()});
    if (op == nullptr) {
        return tokens;
    }
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    const QString resource = dot > 0 ? operationId.left(dot) : QString{};
    for (const auto& ext : op->extractions) {
        const QString name = QString::fromStdString(ext.variableName);
        // variableName is the bare name; reference it namespaced by resource.
        const QString full = name.contains(QLatin1Char('.')) || resource.isEmpty()
                                 ? name
                                 : (resource + QLatin1Char('.') + name);
        tokens.append(QStringLiteral("{{%1}}").arg(full));
    }
    return tokens;
}

QVariantList AppController::extractionPairsFor(const QString& operationId) const {
    QVariantList pairs;
    if (!activeProject().hasProject()) {
        return pairs;
    }
    const auto* op = activeProject().findOperation(engine::OperationId{operationId.toStdString()});
    if (op == nullptr) {
        return pairs;
    }
    for (const auto& ext : op->extractions) {
        QVariantMap row;
        row.insert(QStringLiteral("key"), QString::fromStdString(ext.variableName));
        row.insert(QStringLiteral("value"), QString::fromStdString(ext.sourcePath));
        pairs.append(row);
    }
    return pairs;
}

QVariantList AppController::variableSuggestions(const QString& operationId) const {
    QVariantList out;
    if (!activeProject().hasProject() || !bootstrapper_ || operationId.isEmpty()) {
        return out;
    }
    auto result =
        bootstrapper_->engine().suggestVariables(activeProject().project(),
                                                 engine::OperationId{operationId.toStdString()},
                                                 environment_.toStdString());
    if (!result) {
        return out;
    }
    const auto kindString = [](engine::VariableSuggestion::Kind kind) -> QString {
        switch (kind) {
            case engine::VariableSuggestion::Kind::Extract:
                return QStringLiteral("extract");
            case engine::VariableSuggestion::Kind::ActorToken:
                return QStringLiteral("actor");
            case engine::VariableSuggestion::Kind::EnvVar:
                return QStringLiteral("env");
            case engine::VariableSuggestion::Kind::Secret:
                return QStringLiteral("secret");
            case engine::VariableSuggestion::Kind::Builtin:
                return QStringLiteral("builtin");
        }
        return {};
    };
    for (const auto& suggestion : *result) {
        QVariantMap entry;
        entry.insert(QStringLiteral("token"), QString::fromStdString(suggestion.token));
        entry.insert(QStringLiteral("kind"), kindString(suggestion.kind));
        entry.insert(QStringLiteral("detail"), QString::fromStdString(suggestion.detail));
        out.append(entry);
    }
    return out;
}

std::pair<QString, QString> AppController::findVariableProducer(const QString& token) const {
    if (!activeProject().hasProject() || token.isEmpty()) {
        return {};
    }
    const auto& proj = activeProject().project();
    for (const auto& [resId, resource] : proj.resources) {
        const QString resName = QString::fromStdString(resId.value);
        for (const auto& [opName, op] : resource.operations) {
            for (const auto& ext : op.extractions) {
                const QString var = QString::fromStdString(ext.variableName);
                // Match both the namespaced form (resource.var) and the bare
                // variable name (covers variables named with dots).
                if (token == (resName + QLatin1Char('.') + var) || token == var) {
                    return {resName + QLatin1Char('.') + QString::fromStdString(opName),
                            QString::fromStdString(ext.sourcePath)};
                }
            }
        }
    }
    return {};
}

QStringList AppController::candidateValues(const QString& token) const {
    QStringList out;
    const auto [producerOpId, sourcePath] = findVariableProducer(token);
    if (producerOpId.isEmpty() || sourcePath.isEmpty()) {
        return out;
    }

    // Turn a single-item extract path into its list form so every id surfaces:
    // `$.data[0].id` → `$.data[*].id`.
    static const QRegularExpression indexRe(QStringLiteral("\\[\\d+\\]"));
    QString listPath = sourcePath;
    listPath.replace(indexRe, QStringLiteral("[*]"));

    std::set<QString> seen;
    for (const auto& example : exampleStore_.list(producerOpId)) {
        for (const auto& value :
             engine::extractValues(example.body.toStdString(), listPath.toStdString())) {
            const QString candidate = QString::fromStdString(value);
            if (!candidate.isEmpty() && seen.insert(candidate).second) {
                out.append(candidate);
            }
        }
    }
    return out;
}

QString AppController::producerOpFor(const QString& token) const {
    return findVariableProducer(token).first;
}

QString AppController::responseBodyFor(const QString& operationId) const {
    if (operationId == currentOperationId() && !respBody_.isEmpty()) {
        return respBody_;
    }
    const QList<SavedResponse> saved = exampleStore_.list(operationId);
    return saved.isEmpty() ? QString() : saved.back().body;  // newest last
}

QVariantMap AppController::evaluateExtractionPath(const QString& operationId,
                                                  const QString& path) const {
    const QString body = responseBodyFor(operationId);
    const auto result =
        views::classifyExtractionPath(body.toStdString(), path.trimmed().toStdString());

    QVariantMap out;
    switch (result.state) {
        case views::PathState::Match:
            out.insert(QStringLiteral("state"), QStringLiteral("match"));
            out.insert(QStringLiteral("value"), QString::fromStdString(result.value).left(120));
            break;
        case views::PathState::NoMatch:
            out.insert(QStringLiteral("state"), QStringLiteral("nomatch"));
            break;
        case views::PathState::Neutral:
            out.insert(QStringLiteral("state"), QStringLiteral("neutral"));
            break;
    }
    return out;
}

QStringList AppController::suggestExtractionPaths(const QString& operationId,
                                                  const QString& prefix) const {
    return views::collectJsonPaths(responseBodyFor(operationId), prefix.trimmed());
}

void AppController::setVariableOverride(const QString& token, const QString& value) {
    if (token.isEmpty()) {
        return;
    }
    if (value.isEmpty()) {
        variableOverrides_.remove(token);
    } else {
        variableOverrides_.insert(token, value);
    }

    // Push the full pin set to the run controller and drop the extraction
    // cache so a removed/changed pin can't survive into the next run.
    std::vector<std::pair<std::string, std::string>> overrides;
    overrides.reserve(static_cast<std::size_t>(variableOverrides_.size()));
    for (auto it = variableOverrides_.constBegin(); it != variableOverrides_.constEnd(); ++it) {
        overrides.emplace_back(it.key().toStdString(), it.value().toStdString());
    }
    if (runController_) {
        runController_->setVariableOverrides(std::move(overrides));
        runController_->clearExtractionCache();
    }
    emit variableOverridesChanged();
}

QString AppController::variableOverride(const QString& token) const {
    return variableOverrides_.value(token);
}

void AppController::refreshCandidates(const QString& token) {
    if (!activeProject().hasProject() || runController_ == nullptr || runController_->isRunning()) {
        return;
    }
    const QString producerOpId = findVariableProducer(token).first;
    if (producerOpId.isEmpty()) {
        return;
    }
    // Capture is on by default; the response lands in the auto-save handler.
    pendingCandidateOp_ = producerOpId;
    runController_->run(producerOpId, environment_, false, false);
}

EditableKeyValueModel* AppController::chainExtractModelFor(const QString& operationId) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.extractModelAt(i);
        }
    }
    return nullptr;
}

QStringList AppController::chainForEachOptions(const QString& operationId) const {
    // A step can fan out over any resource produced by another step in the
    // chain. Offer the distinct resources of every other step, discovery order.
    QStringList resources;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        if (id == operationId) {
            continue;
        }
        const QString resource = id.section('.', 0, 0);
        if (!resource.isEmpty() && !resources.contains(resource)) {
            resources.append(resource);
        }
    }
    return resources;
}

QString AppController::chainForEachOver(const QString& operationId) const {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.forEachOverAt(i);
        }
    }
    return {};
}

void AppController::chainSetForEach(const QString& operationId, const QString& overResource) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            chainEditor_.setForEachOver(i, overResource);
            return;
        }
    }
}

bool AppController::chainForEachContinueOnError(const QString& operationId) const {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.forEachContinueOnErrorAt(i);
        }
    }
    return false;
}

void AppController::chainSetForEachContinueOnError(const QString& operationId,
                                                   bool continueOnError) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            chainEditor_.setForEachContinueOnError(i, continueOnError);
            return;
        }
    }
}

bool AppController::addExtraction(const QString& variableName, const QString& sourcePath) {
    if (!activeProject().hasProject() || !hasOperation_) {
        return false;
    }
    const QString opId = currentOperationId();
    const auto* op = activeProject().findOperation(engine::OperationId{opId.toStdString()});
    if (op == nullptr) {
        return false;
    }
    const QString var = variableName.trimmed();
    const QString path = sourcePath.trimmed();
    if (var.isEmpty() || path.isEmpty()) {
        emit notify(QStringLiteral("A variable name and a path are both required."), true);
        return false;
    }

    engine::Operation updated = *op;
    const std::string varStd = var.toStdString();
    const std::string pathStd = path.toStdString();
    // Replace a same-named extraction in place, otherwise append a new one.
    bool replaced = false;
    for (auto& ext : updated.extractions) {
        if (ext.variableName == varStd) {
            ext.sourcePath = pathStd;
            ext.source = sourceForPath(pathStd);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        engine::Extraction extraction;
        extraction.variableName = varStd;
        extraction.sourcePath = pathStd;
        extraction.source = sourceForPath(pathStd);
        updated.extractions.push_back(std::move(extraction));
    }

    QString error;
    if (!activeProject().saveOperation(engine::OperationId{opId.toStdString()}, updated, error)) {
        emit notify(error.isEmpty() ? QStringLiteral("Could not save the variable.") : error, true);
        return false;
    }
    const qsizetype dot = opId.indexOf(QLatin1Char('.'));
    const QString resource = dot > 0 ? opId.left(dot) : opId;
    emit notify(QStringLiteral("Saved variable {{%1.%2}}").arg(resource, var), false);
    return true;
}

QVariantMap AppController::evaluateAssertion(const QString& expression) const {
    QVariantMap out;
    out.insert(QStringLiteral("valid"), false);
    out.insert(QStringLiteral("passed"), false);
    out.insert(QStringLiteral("error"), QString{});

    const QString trimmed = expression.trimmed();
    if (trimmed.isEmpty()) {
        return out;  // nothing to test; UI hides the badge for empty rows
    }

    auto result =
        engine::evaluatePredicate(trimmed.toStdString(), respBody_.toStdString(), respStatus_);
    if (!result) {
        out.insert(QStringLiteral("error"), QString::fromStdString(result.error().detail));
        return out;
    }
    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("passed"), *result);
    return out;
}

QVariantMap AppController::previewFormBody() const {
    QVariantMap out;

    std::map<std::string, std::string> fields;
    for (const auto& [key, value] : editForm_.pairs()) {
        const QString trimmedKey = key.trimmed();
        if (!trimmedKey.isEmpty()) {
            fields[trimmedKey.toStdString()] = value.toStdString();
        }
    }
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : editHeaders_.pairs()) {
        const QString trimmedKey = key.trimmed();
        if (!trimmedKey.isEmpty()) {
            headers[trimmedKey.toStdString()] = value.toStdString();
        }
    }

    auto preview = engine::previewFormBody(fields, headers);
    if (!preview) {
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(preview.error().detail));
        return out;
    }

    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("error"), QString{});
    out.insert(QStringLiteral("multipart"), preview->kind == engine::FormBodyKind::Multipart);
    out.insert(QStringLiteral("contentType"), QString::fromStdString(preview->contentType));
    out.insert(QStringLiteral("totalBytes"), static_cast<qulonglong>(preview->totalBytes));
    QVariantList parts;
    for (const auto& part : preview->parts) {
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(part.name));
        entry.insert(QStringLiteral("isFile"), part.isFile);
        entry.insert(QStringLiteral("filename"), QString::fromStdString(part.filename));
        entry.insert(QStringLiteral("sizeBytes"), static_cast<qulonglong>(part.sizeBytes));
        parts.append(entry);
    }
    out.insert(QStringLiteral("parts"), parts);
    return out;
}

QVariantList AppController::cookieJars() const {
    QVariantList out;
    if (!activeProject().hasProject() || !runController_) {
        return out;
    }
    for (const auto& [actorId, _] : activeProject().project().actors) {
        const auto jar = runController_->cookies(actorId);
        if (jar.empty()) {
            continue;
        }
        QVariantList cookies;
        for (const auto& [name, value] : jar) {
            QVariantMap cookie;
            cookie.insert(QStringLiteral("name"), QString::fromStdString(name));
            cookie.insert(QStringLiteral("value"), QString::fromStdString(value));
            cookies.append(cookie);
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("actor"), QString::fromStdString(actorId.value));
        entry.insert(QStringLiteral("cookies"), cookies);
        out.append(entry);
    }
    return out;
}

int AppController::editParamsCount() const {
    return static_cast<int>(editQuery_.pairs().size());
}

int AppController::editHeadersCount() const {
    return static_cast<int>(editHeaders_.pairs().size());
}

bool AppController::editBodyFilled() const {
    if (editBodyType_ == QStringLiteral("none")) {
        return false;
    }
    return editBodyIsForm_ ? !editForm_.pairs().empty() : !editBody_.trimmed().isEmpty();
}

int AppController::editChainCount() const {
    return static_cast<int>(editDependencies_.dependencies().size() +
                            editExtractions_.pairs().size());
}

int AppController::editAssertionsCount() const {
    int count = 0;
    for (const auto& [expr, name] : editAssertions_.pairs()) {
        if (!expr.trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

QVariantList AppController::chainNodes() const {
    // Static view of the operation's declared dependencies in declared order,
    // then the target itself last (mirrors the old RequestEditorPanel chain
    // preview). In Edit mode the deps come from the live picker so the preview
    // updates as the user wires the chain. Implicit ({{var}}) deps and the
    // full topological order are resolved by the engine after a Dry Run.
    QVariantList nodes;
    if (!activeProject().hasProject() || !hasOperation_) {
        return nodes;
    }
    const QString opId = currentOperationId();
    const auto* op = activeProject().findOperation(engine::OperationId{opId.toStdString()});
    if (op == nullptr) {
        return nodes;
    }

    std::vector<QString> deps;
    if (editing_ && chainFieldsLoaded_) {
        for (const auto& dep : editDependencies_.dependencies()) {
            deps.emplace_back(QString::fromStdString(dep));
        }
    } else {
        for (const auto& dep : op->explicitDependencies) {
            deps.emplace_back(QString::fromStdString(dep.value));
        }
    }
    if (deps.empty()) {
        return nodes;
    }

    const auto nodeFor = [this](const QString& id, bool isTarget) {
        QVariantMap node;
        node.insert(QStringLiteral("operationId"), id);
        const auto* depOp = activeProject().findOperation(engine::OperationId{id.toStdString()});
        node.insert(QStringLiteral("method"),
                    depOp != nullptr ? methodLabel(depOp->method) : QString{});
        node.insert(QStringLiteral("isTarget"), isTarget);
        return node;
    };
    for (const QString& dep : deps) {
        nodes.append(nodeFor(dep, /*isTarget=*/false));
    }
    nodes.append(nodeFor(opId, /*isTarget=*/true));
    return nodes;
}

QVariantMap AppController::chainGraph() const {
    // Draw the target's resolved dependency chain. The engine is the single
    // source of truth for resolution: resolvePlan() returns the topological
    // execution order plus the explicit/implicit edges (each implicit edge
    // tagged with the variable that flows along it). We only lay the result
    // out and translate it to the QML node/edge shape — no dependency
    // re-derivation here, so the drawn graph always matches what the engine
    // actually executes.
    QVariantMap graph;
    if (!activeProject().hasProject() || !hasOperation_ || !bootstrapper_) {
        return graph;
    }
    const QString targetId = currentOperationId();
    const engine::OperationId targetOpId{targetId.toStdString()};
    if (activeProject().findOperation(targetOpId) == nullptr) {
        return graph;
    }

    // In edit mode the chain picker holds unsaved depends_on edits. Resolve
    // against a patched copy so the preview tracks the live wiring; otherwise
    // resolve the persisted project directly.
    engine::Project patched;
    const engine::Project* proj = &activeProject().project();
    if (editing_ && chainFieldsLoaded_) {
        patched = activeProject().project();
        const qsizetype dot = targetId.indexOf(QLatin1Char('.'));
        if (dot > 0) {
            const engine::ResourceId resId{targetId.left(dot).toStdString()};
            const std::string opName = targetId.mid(dot + 1).toStdString();
            if (auto resIt = patched.resources.find(resId); resIt != patched.resources.end()) {
                if (auto opIt = resIt->second.operations.find(opName);
                    opIt != resIt->second.operations.end()) {
                    opIt->second.explicitDependencies.clear();
                    for (const auto& dep : editDependencies_.dependencies()) {
                        opIt->second.explicitDependencies.push_back(engine::OperationId{dep});
                    }
                }
            }
        }
        proj = &patched;
    }

    const auto plan = bootstrapper_->engine().resolvePlan(*proj, targetOpId);
    if (!plan || plan->order.size() <= 1) {
        // No chain to draw (or an edit-time cycle); ChainView shows empty text.
        return graph;
    }

    // Node index = position in the engine's topological order (deps first,
    // target last).
    QHash<QString, int> indexOf;
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(plan->order.size()));
    for (const auto& opId : plan->order) {
        const QString id = QString::fromStdString(opId.value);
        indexOf.insert(id, static_cast<int>(ids.size()));
        ids.append(id);
    }

    // Aggregate engine edges by (producer → consumer) pair so a pair draws
    // once, not in parallel: an explicit edge wins (solid, no label); otherwise
    // join the implicit edges' flowing variables into one labeled edge.
    struct EdgeAgg {
        bool isExplicit{false};
        QStringList vars;
    };
    std::map<std::pair<int, int>, EdgeAgg> edgeAgg;
    QHash<QString, QStringList> depsOf;  // consumer id → producer ids
    for (const auto& edge : plan->edges) {
        const QString consumer = QString::fromStdString(edge.consumer.value);
        const QString producer = QString::fromStdString(edge.producer.value);
        const auto cIt = indexOf.constFind(consumer);
        const auto pIt = indexOf.constFind(producer);
        if (cIt == indexOf.constEnd() || pIt == indexOf.constEnd()) {
            continue;
        }
        // Layout/edge direction is prerequisite → dependent: from=producer.
        EdgeAgg& agg = edgeAgg[std::pair<int, int>{pIt.value(), cIt.value()}];
        if (edge.implicit) {
            if (!edge.variable.empty()) {
                agg.vars.append(
                    QStringLiteral("{{%1}}").arg(QString::fromStdString(edge.variable)));
            }
        } else {
            agg.isExplicit = true;
        }
        QStringList& producers = depsOf[consumer];
        if (!producers.contains(producer)) {
            producers.append(producer);
        }
    }

    std::vector<std::pair<int, int>> layoutEdges;
    layoutEdges.reserve(edgeAgg.size());
    for (const auto& [key, agg] : edgeAgg) {
        layoutEdges.push_back(key);
    }

    layout::LayoutOptions options;
    options.nodeWidth = 200.0;
    options.nodeHeight = 38.0;
    options.hGap = 20.0;
    options.vGap = 36.0;
    const layout::LayoutResult laid =
        layout::layeredLayout(static_cast<int>(ids.size()), layoutEdges, options);

    // Resolve a node's operation against the same project the plan came from,
    // so an edit-mode target reflects the patched copy.
    const auto findOp = [proj](const QString& id) -> const engine::Operation* {
        const qsizetype dot = id.indexOf(QLatin1Char('.'));
        if (dot <= 0) {
            return nullptr;
        }
        const auto resIt = proj->resources.find(engine::ResourceId{id.left(dot).toStdString()});
        if (resIt == proj->resources.end()) {
            return nullptr;
        }
        const auto opIt = resIt->second.operations.find(id.mid(dot + 1).toStdString());
        return opIt == resIt->second.operations.end() ? nullptr : &opIt->second;
    };

    QVariantList nodeList;
    for (int i = 0; i < ids.size(); ++i) {
        const QString& id = ids.at(i);
        const auto* op = findOp(id);
        QVariantMap node;
        node.insert(QStringLiteral("operationId"), id);
        node.insert(QStringLiteral("method"), op != nullptr ? methodLabel(op->method) : QString{});
        node.insert(QStringLiteral("isTarget"), id == targetId);
        node.insert(QStringLiteral("x"), laid.nodes[static_cast<std::size_t>(i)].x);
        node.insert(QStringLiteral("y"), laid.nodes[static_cast<std::size_t>(i)].y);
        // Detail surfaced when a node is clicked in the graph.
        node.insert(QStringLiteral("path"),
                    op != nullptr ? QString::fromStdString(op->pathTemplate) : QString{});
        node.insert(QStringLiteral("actor"),
                    op != nullptr ? QString::fromStdString(op->actor.value) : QString{});
        QStringList extracts;
        if (op != nullptr) {
            for (const auto& extraction : op->extractions) {
                extracts.append(QString::fromStdString(extraction.variableName));
            }
        }
        node.insert(QStringLiteral("extracts"), extracts);
        QStringList deps = depsOf.value(id);
        deps.sort();
        node.insert(QStringLiteral("deps"), deps);
        nodeList.append(node);
    }

    QVariantList edgeList;
    for (auto& [key, agg] : edgeAgg) {
        agg.vars.sort();
        QVariantMap edge;
        edge.insert(QStringLiteral("from"), key.first);
        edge.insert(QStringLiteral("to"), key.second);
        edge.insert(QStringLiteral("explicit"), agg.isExplicit);
        edge.insert(QStringLiteral("label"),
                    agg.isExplicit ? QString{} : agg.vars.join(QStringLiteral(", ")));
        edgeList.append(edge);
    }

    graph.insert(QStringLiteral("nodes"), nodeList);
    graph.insert(QStringLiteral("edges"), edgeList);
    graph.insert(QStringLiteral("width"), laid.width);
    graph.insert(QStringLiteral("height"), laid.height);
    graph.insert(QStringLiteral("nodeWidth"), options.nodeWidth);
    graph.insert(QStringLiteral("nodeHeight"), options.nodeHeight);
    return graph;
}

QVariantList AppController::executionPreview() const {
    QVariantList preview;
    if (!activeProject().hasProject() || !hasOperation_ || bootstrapper_ == nullptr) {
        return preview;
    }
    const engine::OperationId targetOpId{currentOperationId().toStdString()};
    const engine::Project& project = activeProject().project();
    if (project.resources.empty() || activeProject().findOperation(targetOpId) == nullptr) {
        return preview;
    }

    // Resolved against the persisted project, not the edit-mode patched copy the
    // chain graph builds: this previews what a run would do now. An unresolvable
    // chain (cycle) yields an empty list, and the caller keeps its empty state.
    const auto plan = bootstrapper_->engine().resolvePlan(project, targetOpId);
    if (!plan) {
        return preview;
    }

    const std::vector<PreviewStep> steps = buildExecutionPreview(*plan, project);
    preview.reserve(static_cast<qsizetype>(steps.size()));
    for (const PreviewStep& step : steps) {
        QVariantList produces;
        produces.reserve(static_cast<qsizetype>(step.produces.size()));
        for (const PreviewOutput& output : step.produces) {
            produces.append(QVariantMap{{QStringLiteral("variable"), output.variable},
                                        {QStringLiteral("sourcePath"), output.sourcePath}});
        }
        QVariantList expectStatus;
        expectStatus.reserve(step.expectStatus.size());
        for (const int status : step.expectStatus) {
            expectStatus.append(status);
        }

        preview.append(QVariantMap{{QStringLiteral("number"), step.number},
                                   {QStringLiteral("operationId"), step.operationId},
                                   {QStringLiteral("method"), step.method},
                                   {QStringLiteral("path"), step.pathTemplate},
                                   {QStringLiteral("actor"), step.actor},
                                   {QStringLiteral("isTarget"), step.isTarget},
                                   {QStringLiteral("dependsOn"), step.dependsOn},
                                   {QStringLiteral("produces"), produces},
                                   {QStringLiteral("expectStatus"), expectStatus}});
    }
    return preview;
}

QStringList AppController::extractionConsumers(const QString& producerOperationId,
                                               const QString& variable) const {
    if (!activeProject().hasProject() || !hasOperation_ || bootstrapper_ == nullptr) {
        return {};
    }
    if (producerOperationId.isEmpty() || variable.isEmpty()) {
        return {};
    }
    const engine::OperationId targetOpId{currentOperationId().toStdString()};
    const engine::Project& project = activeProject().project();
    if (activeProject().findOperation(targetOpId) == nullptr) {
        return {};
    }

    // ponytail: resolves the plan per call, so a run with many missed extractions
    // re-resolves once per row. Measured cheap on real projects (the whole
    // marketplace sample resolves in well under a second). If a large project
    // ever makes this show up, cache the plan per (target, chainChanged) instead
    // of widening the API.
    const auto plan = bootstrapper_->engine().resolvePlan(project, targetOpId);
    if (!plan) {
        return {};
    }
    return consumersOfVariable(*plan, producerOperationId, variable);
}

QVariantMap AppController::chainStatus() const {
    QVariantMap status;
    for (auto it = chainStatus_.constBegin(); it != chainStatus_.constEnd(); ++it) {
        status.insert(it.key(), it.value());
    }
    return status;
}

void AppController::prepareChainEditor() {
    if (!activeProject().hasProject() || !hasOperation_) {
        chainEditor_.rebuild({});
        return;
    }
    const QString targetId = currentOperationId();
    const auto* targetOp =
        activeProject().findOperation(engine::OperationId{targetId.toStdString()});
    if (targetOp == nullptr) {
        chainEditor_.rebuild({});
        return;
    }

    // BFS the declared transitive dependency closure, in discovery order.
    QStringList ids;
    QHash<QString, bool> seen;
    std::queue<QString> pending;
    ids.append(targetId);
    seen.insert(targetId, true);
    pending.push(targetId);
    while (!pending.empty()) {
        const QString id = pending.front();
        pending.pop();
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        for (const auto& dep : op->explicitDependencies) {
            const QString depId = QString::fromStdString(dep.value);
            if (!seen.contains(depId)) {
                seen.insert(depId, true);
                ids.append(depId);
                pending.push(depId);
            }
        }
    }

    const QStringList allIds = operationIds();
    std::vector<ChainEditorModel::OpSeed> seeds;
    seeds.reserve(static_cast<std::size_t>(ids.size()));
    for (const QString& id : ids) {
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        ChainEditorModel::OpSeed seed;
        seed.operationId = id;
        seed.method = op != nullptr ? methodLabel(op->method) : QString{};
        seed.isTarget = (id == targetId);
        if (op != nullptr) {
            for (const auto& dep : op->explicitDependencies) {
                seed.dependencies.push_back(dep.value);
            }
            for (const auto& ext : op->extractions) {
                seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                              QString::fromStdString(ext.sourcePath));
            }
            if (op->forEach) {
                seed.forEachOver = QString::fromStdString(op->forEach->over.value);
                seed.forEachContinueOnError = op->forEach->continueOnError;
            }
        }
        for (const QString& candidate : allIds) {
            if (candidate != id) {
                seed.candidates.append(candidate);
            }
        }
        seeds.push_back(std::move(seed));
    }
    chainEditor_.rebuild(seeds);
}

void AppController::syncChainEditorMembership() {
    if (!activeProject().hasProject() || !hasOperation_ || chainEditor_.count() == 0) {
        return;
    }
    const QString targetId = currentOperationId();

    // Snapshot current edits so a rebuild preserves in-progress work.
    QHash<QString, std::vector<std::string>> editedDeps;
    QHash<QString, std::vector<std::pair<QString, QString>>> editedExtracts;
    QHash<QString, QString> editedForEach;
    QHash<QString, bool> editedForEachContinue;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        editedDeps.insert(id, chainEditor_.depModelAt(i)->dependencies());
        editedExtracts.insert(id, chainEditor_.extractModelAt(i)->pairs());
        editedForEach.insert(id, chainEditor_.forEachOverAt(i));
        editedForEachContinue.insert(id, chainEditor_.forEachContinueOnErrorAt(i));
    }

    // BFS the transitive closure from the target using edited deps where we
    // have them, falling back to the saved project for not-yet-edited steps.
    QStringList ids;
    QHash<QString, bool> seen;
    std::queue<QString> pending;
    ids.append(targetId);
    seen.insert(targetId, true);
    pending.push(targetId);
    while (!pending.empty()) {
        const QString id = pending.front();
        pending.pop();
        std::vector<std::string> deps;
        if (editedDeps.contains(id)) {
            deps = editedDeps.value(id);
        } else if (const auto* op =
                       activeProject().findOperation(engine::OperationId{id.toStdString()})) {
            for (const auto& dep : op->explicitDependencies) {
                deps.push_back(dep.value);
            }
        }
        for (const auto& dep : deps) {
            const QString depId = QString::fromStdString(dep);
            if (!seen.contains(depId)) {
                seen.insert(depId, true);
                ids.append(depId);
                pending.push(depId);
            }
        }
    }

    // Membership unchanged → live models are already correct, skip the rebuild
    // (avoids resetting focus and re-entrancy).
    QSet<QString> current;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        current.insert(chainEditor_.operationIdAt(i));
    }
    const QSet<QString> wanted(ids.cbegin(), ids.cend());
    if (current == wanted) {
        return;
    }

    const QStringList allIds = operationIds();
    std::vector<ChainEditorModel::OpSeed> seeds;
    seeds.reserve(static_cast<std::size_t>(ids.size()));
    for (const QString& id : ids) {
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        ChainEditorModel::OpSeed seed;
        seed.operationId = id;
        seed.method = op != nullptr ? methodLabel(op->method) : QString{};
        seed.isTarget = (id == targetId);
        if (editedDeps.contains(id)) {
            seed.dependencies = editedDeps.value(id);
        } else if (op != nullptr) {
            for (const auto& dep : op->explicitDependencies) {
                seed.dependencies.push_back(dep.value);
            }
        }
        if (editedExtracts.contains(id)) {
            seed.extractions = editedExtracts.value(id);
        } else if (op != nullptr) {
            for (const auto& ext : op->extractions) {
                seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                              QString::fromStdString(ext.sourcePath));
            }
        }
        if (editedForEach.contains(id)) {
            seed.forEachOver = editedForEach.value(id);
        } else if (op != nullptr && op->forEach) {
            seed.forEachOver = QString::fromStdString(op->forEach->over.value);
        }
        if (editedForEachContinue.contains(id)) {
            seed.forEachContinueOnError = editedForEachContinue.value(id);
        } else if (op != nullptr && op->forEach) {
            seed.forEachContinueOnError = op->forEach->continueOnError;
        }
        for (const QString& candidate : allIds) {
            if (candidate != id) {
                seed.candidates.append(candidate);
            }
        }
        seeds.push_back(std::move(seed));
    }
    chainEditor_.rebuild(seeds);
}

void AppController::chainAddDependency(const QString& operationId) {
    if (operationId.isEmpty() || chainEditor_.count() == 0) {
        return;
    }
    const QString targetId = currentOperationId();
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == targetId) {
            auto* deps = chainEditor_.depModelAt(i);
            const int ghost = deps->rowCount() - 1;
            deps->setSelection(ghost >= 0 ? ghost : 0, operationId);
            break;
        }
    }
    syncChainEditorMembership();
}

void AppController::chainRemoveStep(const QString& operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    // Drop it as a dependency of every step so it is no longer referenced.
    for (int i = 0; i < chainEditor_.count(); ++i) {
        auto* deps = chainEditor_.depModelAt(i);
        for (int row = 0; row < deps->rowCount(); ++row) {
            const QString value =
                deps->data(deps->index(row, 0), DependencyEditModel::ValueRole).toString();
            if (value == operationId) {
                deps->removeRow(row);
                break;
            }
        }
    }
    syncChainEditorMembership();
}

bool AppController::saveChainEdits() {
    if (!activeProject().hasProject()) {
        return false;
    }
    std::map<std::string, engine::Operation> updates;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        engine::Operation updated = *op;  // copy; patch only the chain fields

        updated.explicitDependencies.clear();
        for (const auto& dep : chainEditor_.depModelAt(i)->dependencies()) {
            updated.explicitDependencies.push_back(engine::OperationId{dep});
        }

        // Preserve each extraction's source kind by variable name; new rows
        // default to JSONPath (the common case).
        std::map<std::string, engine::Extraction::Source> sourceByVar;
        for (const auto& ext : op->extractions) {
            sourceByVar[ext.variableName] = ext.source;
        }
        updated.extractions.clear();
        for (const auto& [variable, sourcePath] : chainEditor_.extractModelAt(i)->pairs()) {
            const QString var = variable.trimmed();
            const QString path = sourcePath.trimmed();
            if (var.isEmpty() && path.isEmpty()) {
                continue;
            }
            engine::Extraction extraction;
            extraction.variableName = var.toStdString();
            extraction.sourcePath = path.toStdString();
            const auto found = sourceByVar.find(extraction.variableName);
            extraction.source =
                found != sourceByVar.end() ? found->second : engine::Extraction::Source::JsonPath;
            updated.extractions.push_back(std::move(extraction));
        }

        // For-each fan-out: set or clear based on the chain editor's choice.
        const QString overResource = chainEditor_.forEachOverAt(i).trimmed();
        if (overResource.isEmpty()) {
            updated.forEach.reset();
        } else {
            engine::ForEach forEach{engine::ResourceId{overResource.toStdString()}};
            forEach.continueOnError = chainEditor_.forEachContinueOnErrorAt(i);
            updated.forEach = forEach;
        }

        updates.emplace(id.toStdString(), std::move(updated));
    }

    QString error;
    if (!activeProject().saveOperations(updates, error)) {
        emit notify(error.isEmpty() ? QStringLiteral("Could not save the chain.") : error, true);
        return false;
    }
    emit notify(QStringLiteral("Chain saved."), false);

    // Save chain rewrote the target's depends_on / extract on disk; refresh the
    // endpoint editor's edit-mode models so a later endpoint Save (which writes
    // from those models) reflects — rather than clobbers — what we just saved.
    if (editing_) {
        seedEditChainFromProject();
        emit editChanged();
        emit chainChanged();
    }
    return true;
}

void AppController::seedEditChainFromProject() {
    if (!activeProject().hasProject()) {
        return;
    }
    const auto* op =
        activeProject().findOperation(engine::OperationId{currentOperationId().toStdString()});
    if (op == nullptr) {
        return;
    }
    editDependencies_.setCandidates(editDependencyCandidates());
    std::vector<std::string> deps;
    deps.reserve(op->explicitDependencies.size());
    for (const auto& dep : op->explicitDependencies) {
        deps.push_back(dep.value);
    }
    editDependencies_.setDependencies(deps);

    std::vector<std::pair<QString, QString>> extractRows;
    extractRows.reserve(op->extractions.size());
    for (const auto& ext : op->extractions) {
        extractRows.emplace_back(QString::fromStdString(ext.variableName),
                                 QString::fromStdString(ext.sourcePath));
    }
    editExtractions_.setPairs(std::move(extractRows));

    std::vector<std::pair<QString, QString>> assertRows;
    assertRows.reserve(op->assertions.size());
    for (const auto& a : op->assertions) {
        assertRows.emplace_back(QString::fromStdString(a.expr),
                                a.name ? QString::fromStdString(*a.name) : QString{});
    }
    editAssertions_.setPairs(std::move(assertRows));
}

}  // namespace reqloom::desktop::qml
