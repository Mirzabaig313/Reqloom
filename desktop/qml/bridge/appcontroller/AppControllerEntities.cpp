// AppController — actor, environment, history, and resource/operation CRUD.
// Split from AppController.cpp; see AppController.h.
#include "AppController.h"

#include "ThemeController.h"
#include "application/EnvironmentSettings.h"
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

namespace {

[[nodiscard]] engine::AuthStrategy authStrategyFromLabel(const QString& label) {
    if (label == QStringLiteral("Multi-step login chain")) {
        return engine::AuthStrategy::Chain;
    }
    if (label == QStringLiteral("Basic Auth")) {
        return engine::AuthStrategy::Basic;
    }
    if (label == QStringLiteral("API Key")) {
        return engine::AuthStrategy::ApiKey;
    }
    if (label == QStringLiteral("OAuth 2.0 (Client Credentials)")) {
        return engine::AuthStrategy::OAuth2ClientCredentials;
    }
    if (label == QStringLiteral("OAuth 2.0 (Password)")) {
        return engine::AuthStrategy::OAuth2Password;
    }
    if (label == QStringLiteral("OAuth 1.0 (HMAC-SHA1)")) {
        return engine::AuthStrategy::OAuth1;
    }
    if (label == QStringLiteral("AWS Signature v4")) {
        return engine::AuthStrategy::AwsSigV4;
    }
    return engine::AuthStrategy::Simple;
}

[[nodiscard]] engine::HttpMethod httpMethodFromLabel(const QString& method) {
    const QString upper = method.trimmed().toUpper();
    if (upper == QStringLiteral("GET")) {
        return engine::HttpMethod::Get;
    }
    if (upper == QStringLiteral("PUT")) {
        return engine::HttpMethod::Put;
    }
    if (upper == QStringLiteral("PATCH")) {
        return engine::HttpMethod::Patch;
    }
    if (upper == QStringLiteral("DELETE")) {
        return engine::HttpMethod::Delete;
    }
    if (upper == QStringLiteral("HEAD")) {
        return engine::HttpMethod::Head;
    }
    if (upper == QStringLiteral("OPTIONS")) {
        return engine::HttpMethod::Options;
    }
    return engine::HttpMethod::Post;
}

[[nodiscard]] std::vector<engine::Extraction> extractionsFromPairs(
    const std::vector<std::pair<QString, QString>>& pairs) {
    std::vector<engine::Extraction> out;
    for (const auto& [key, value] : pairs) {
        if (key.isEmpty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = key.toStdString();
        ext.sourcePath = value.toStdString();
        ext.source = engine::Extraction::Source::JsonPath;
        out.push_back(std::move(ext));
    }
    return out;
}

}  // namespace

QStringList AppController::actorStrategies() const {
    return {QStringLiteral("Basic Auth"),
            QStringLiteral("API Key"),
            QStringLiteral("OAuth 2.0 (Client Credentials)"),
            QStringLiteral("OAuth 2.0 (Password)"),
            QStringLiteral("OAuth 1.0 (HMAC-SHA1)"),
            QStringLiteral("AWS Signature v4"),
            QStringLiteral("Single-step login"),
            QStringLiteral("Multi-step login chain")};
}

QString AppController::actorDescription(const QString& actorId) const {
    if (!activeProject().hasProject()) {
        return {};
    }
    const auto& actors = activeProject().project().actors;
    const auto it = actors.find(engine::ActorId{actorId.toStdString()});
    return it == actors.end() ? QString{} : QString::fromStdString(it->second.description);
}

void AppController::prepareNewActor() {
    actorAuthMethod_ = QStringLiteral("POST");
    actorAuthPath_.clear();
    actorAuthBody_.clear();
    actorAuthExpect_.clear();
    actorHasRefresh_ = false;
    actorRefreshMethod_ = QStringLiteral("POST");
    actorRefreshPath_.clear();
    actorRefreshBody_.clear();
    actorConfig_.clearRows();
    actorAuthExtract_.clearRows();
    actorRefreshExtract_.clearRows();
    // One blank login step so the N-step editor always has a row to edit.
    actorAuthSteps_.rebuild({AuthStepListModel::StepSeed{
        QStringLiteral("login"), QStringLiteral("POST"), {}, {}, {}, {}}});
    emit actorEditChanged();
}

void AppController::prepareEditActor(const QString& actorId) {
    actorAuthMethod_ = QStringLiteral("POST");
    actorAuthPath_.clear();
    actorAuthBody_.clear();
    actorAuthExpect_.clear();
    actorHasRefresh_ = false;
    actorRefreshMethod_ = QStringLiteral("POST");
    actorRefreshPath_.clear();
    actorRefreshBody_.clear();

    std::vector<std::pair<QString, QString>> config;
    std::vector<std::pair<QString, QString>> authEx;
    std::vector<std::pair<QString, QString>> refreshEx;
    std::vector<AuthStepListModel::StepSeed> stepSeeds;

    if (activeProject().hasProject()) {
        const auto& actors = activeProject().project().actors;
        const auto it = actors.find(engine::ActorId{actorId.toStdString()});
        if (it != actors.end()) {
            const engine::Actor& actor = it->second;
            for (const auto& [key, value] : actor.authConfig) {
                config.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
            }
            // Full N-step chain for the step-list editor.
            for (const auto& step : actor.authSteps) {
                AuthStepListModel::StepSeed seed;
                seed.id = QString::fromStdString(step.id);
                seed.method = methodLabel(step.method);
                seed.path = QString::fromStdString(step.pathTemplate);
                seed.body =
                    step.bodyTemplate ? QString::fromStdString(*step.bodyTemplate) : QString{};
                seed.expect = step.expectStatus ? QString::number(*step.expectStatus) : QString{};
                for (const auto& ext : step.extractions) {
                    seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                                  QString::fromStdString(ext.sourcePath));
                }
                stepSeeds.push_back(std::move(seed));
            }
            if (!actor.authSteps.empty()) {
                const engine::AuthStep& step = actor.authSteps.front();
                actorAuthMethod_ = methodLabel(step.method);
                actorAuthPath_ = QString::fromStdString(step.pathTemplate);
                actorAuthBody_ =
                    step.bodyTemplate ? QString::fromStdString(*step.bodyTemplate) : QString{};
                actorAuthExpect_ =
                    step.expectStatus ? QString::number(*step.expectStatus) : QString{};
                for (const auto& ext : step.extractions) {
                    authEx.emplace_back(QString::fromStdString(ext.variableName),
                                        QString::fromStdString(ext.sourcePath));
                }
            }
            if (actor.refresh) {
                actorHasRefresh_ = true;
                actorRefreshMethod_ = methodLabel(actor.refresh->method);
                actorRefreshPath_ = QString::fromStdString(actor.refresh->pathTemplate);
                actorRefreshBody_ = actor.refresh->bodyTemplate
                                        ? QString::fromStdString(*actor.refresh->bodyTemplate)
                                        : QString{};
                for (const auto& ext : actor.refresh->extractions) {
                    refreshEx.emplace_back(QString::fromStdString(ext.variableName),
                                           QString::fromStdString(ext.sourcePath));
                }
            }
        }
    }
    actorConfig_.setPairs(std::move(config));
    actorAuthExtract_.setPairs(std::move(authEx));
    actorRefreshExtract_.setPairs(std::move(refreshEx));
    // Guarantee the step editor always has a row (a step-based actor with no
    // steps yet, or a non-step strategy the user may switch to step-based).
    if (stepSeeds.empty()) {
        stepSeeds.push_back(AuthStepListModel::StepSeed{
            QStringLiteral("login"), QStringLiteral("POST"), {}, {}, {}, {}});
    }
    actorAuthSteps_.rebuild(stepSeeds);
    emit actorEditChanged();
}

bool AppController::saveActorEdits(const QString& originalId,
                                   const QString& name,
                                   const QString& strategyLabel,
                                   const QString& description,
                                   const QString& authMethod,
                                   const QString& authPath,
                                   const QString& authBody,
                                   const QString& authExpect,
                                   bool refreshEnabled,
                                   const QString& refreshMethod,
                                   const QString& refreshPath,
                                   const QString& refreshBody) {
    // Start from the existing actor (preserving inject headers, session TTL and
    // any chain steps beyond the first) when editing.
    engine::Actor actor;
    if (!originalId.isEmpty() && activeProject().hasProject()) {
        const auto& actors = activeProject().project().actors;
        const auto it = actors.find(engine::ActorId{originalId.toStdString()});
        if (it != actors.end()) {
            actor = it->second;
        }
    }
    actor.id = engine::ActorId{name.trimmed().toStdString()};
    actor.description = description.trimmed().toStdString();
    const engine::AuthStrategy strategy = authStrategyFromLabel(strategyLabel);
    actor.strategy = strategy;

    std::map<std::string, std::string> config;
    for (const auto& [key, value] : actorConfig_.pairs()) {
        if (!key.isEmpty()) {
            config.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }
    actor.authConfig = std::move(config);

    const bool stepBased =
        (strategy == engine::AuthStrategy::Simple || strategy == engine::AuthStrategy::Chain);
    if (stepBased) {
        engine::AuthStep step;
        if (!actor.authSteps.empty()) {
            step = actor.authSteps.front();  // keep id + headers
        }
        if (step.id.empty()) {
            step.id = "login";
        }
        step.method = httpMethodFromLabel(authMethod);
        step.pathTemplate = authPath.trimmed().toStdString();
        const QString body = authBody.trimmed();
        step.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                           : std::optional<std::string>{body.toStdString()};
        bool okExpect = false;
        const int expect = authExpect.trimmed().toInt(&okExpect);
        step.expectStatus = okExpect ? std::optional<int>{expect} : std::optional<int>{};
        step.extractions = extractionsFromPairs(actorAuthExtract_.pairs());
        if (actor.authSteps.empty()) {
            actor.authSteps.push_back(std::move(step));
        } else {
            actor.authSteps.front() = std::move(step);
        }
    } else {
        actor.authSteps.clear();
    }

    if (refreshEnabled) {
        engine::SessionRefresh refresh;
        if (actor.refresh) {
            refresh = *actor.refresh;
        }
        refresh.method = httpMethodFromLabel(refreshMethod);
        refresh.pathTemplate = refreshPath.trimmed().toStdString();
        const QString body = refreshBody.trimmed();
        refresh.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                              : std::optional<std::string>{body.toStdString()};
        refresh.extractions = extractionsFromPairs(actorRefreshExtract_.pairs());
        actor.refresh = std::move(refresh);
    } else {
        actor.refresh.reset();
    }

    QString error;
    if (activeProject().saveActor(originalId, actor, error)) {
        emit notify(QStringLiteral("Saved actor “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

bool AppController::saveActorInline(const QString& originalId,
                                    const QString& name,
                                    const QString& strategyLabel,
                                    const QString& description,
                                    bool refreshEnabled,
                                    const QString& refreshMethod,
                                    const QString& refreshPath,
                                    const QString& refreshBody) {
    // Start from the existing actor (preserving inject headers + session TTL).
    engine::Actor actor;
    if (!originalId.isEmpty() && activeProject().hasProject()) {
        const auto& actors = activeProject().project().actors;
        const auto it = actors.find(engine::ActorId{originalId.toStdString()});
        if (it != actors.end()) {
            actor = it->second;
        }
    }
    actor.id = engine::ActorId{name.trimmed().toStdString()};
    actor.description = description.trimmed().toStdString();
    const engine::AuthStrategy strategy = authStrategyFromLabel(strategyLabel);
    actor.strategy = strategy;

    std::map<std::string, std::string> config;
    for (const auto& [key, value] : actorConfig_.pairs()) {
        if (!key.isEmpty()) {
            config.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }
    actor.authConfig = std::move(config);

    const bool stepBased =
        (strategy == engine::AuthStrategy::Simple || strategy == engine::AuthStrategy::Chain);
    if (stepBased) {
        // Rebuild the whole login chain from the step-list editor.
        actor.authSteps.clear();
        for (int i = 0; i < actorAuthSteps_.count(); ++i) {
            engine::AuthStep step;
            step.id = actorAuthSteps_.idAt(i).trimmed().toStdString();
            if (step.id.empty()) {
                step.id = (i == 0) ? std::string{"login"} : "step" + std::to_string(i + 1);
            }
            step.method = httpMethodFromLabel(actorAuthSteps_.methodAt(i));
            step.pathTemplate = actorAuthSteps_.pathAt(i).trimmed().toStdString();
            const QString body = actorAuthSteps_.bodyAt(i).trimmed();
            step.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                               : std::optional<std::string>{body.toStdString()};
            bool okExpect = false;
            const int expect = actorAuthSteps_.expectAt(i).trimmed().toInt(&okExpect);
            step.expectStatus = okExpect ? std::optional<int>{expect} : std::optional<int>{};
            const auto* extractModel = actorAuthSteps_.extractModelAt(i);
            if (extractModel != nullptr) {
                step.extractions = extractionsFromPairs(extractModel->pairs());
            }
            actor.authSteps.push_back(std::move(step));
        }
    } else {
        actor.authSteps.clear();
    }

    if (refreshEnabled) {
        engine::SessionRefresh refresh;
        if (actor.refresh) {
            refresh = *actor.refresh;
        }
        refresh.method = httpMethodFromLabel(refreshMethod);
        refresh.pathTemplate = refreshPath.trimmed().toStdString();
        const QString body = refreshBody.trimmed();
        refresh.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                              : std::optional<std::string>{body.toStdString()};
        refresh.extractions = extractionsFromPairs(actorRefreshExtract_.pairs());
        actor.refresh = std::move(refresh);
    } else {
        actor.refresh.reset();
    }

    QString error;
    if (activeProject().saveActor(originalId, actor, error)) {
        emit notify(QStringLiteral("Saved actor “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteActor(const QString& actorId) {
    QString error;
    if (activeProject().deleteActor(engine::ActorId{actorId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted actor “%1”").arg(actorId), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::prepareNewEnvironment() {
    envVars_.clearRows();
    editEnvBaseUrl_.clear();
    emit editEnvBaseUrlChanged();
}

void AppController::setEditEnvBaseUrl(const QString& url) {
    if (url == editEnvBaseUrl_) {
        return;
    }
    editEnvBaseUrl_ = url;
    emit editEnvBaseUrlChanged();
}

void AppController::prepareEditEnvironment(const QString& name) {
    std::vector<std::pair<QString, QString>> pairs;
    editEnvBaseUrl_.clear();
    if (activeProject().hasProject()) {
        const auto& envs = activeProject().project().environments;
        const auto it = envs.find(name.toStdString());
        if (it != envs.end()) {
            for (const auto& [key, value] : it->second) {
                // baseUrl is surfaced in its own dedicated field, not the table.
                if (key == "baseUrl") {
                    editEnvBaseUrl_ = QString::fromStdString(value);
                    continue;
                }
                pairs.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
            }
        }
    }
    envVars_.setPairs(std::move(pairs));
    emit editEnvBaseUrlChanged();
}

bool AppController::saveEnvironmentEdits(const QString& originalName, const QString& name) {
    std::map<std::string, std::string> variables;
    for (const auto& [key, value] : envVars_.pairs()) {
        // Skip a stray baseUrl row — the dedicated field is authoritative.
        if (!key.isEmpty() && key != QStringLiteral("baseUrl")) {
            variables.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }
    const QString baseUrl = editEnvBaseUrl_.trimmed();
    if (!baseUrl.isEmpty()) {
        variables.insert_or_assign(std::string{"baseUrl"}, baseUrl.toStdString());
    }
    QString error;
    if (activeProject().saveEnvironment(originalName, name, variables, error)) {
        setEnvironment(name.trimmed());
        emit notify(QStringLiteral("Saved environment “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteEnvironment(const QString& name) {
    QString error;
    if (activeProject().deleteEnvironment(name, error)) {
        emit notify(QStringLiteral("Deleted environment “%1”").arg(name), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::openProjectHistory(const QString& projectRoot) {
    if (!bootstrapper_) {
        return;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty() || projectRoot.isEmpty()) {
        return;
    }
    // Hash the canonical project path so each project maps to a stable,
    // filesystem-safe database name without leaking the path or colliding.
    const QString canonical = QFileInfo(projectRoot).canonicalFilePath();
    const QString key = canonical.isEmpty() ? projectRoot : canonical;
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir{base}.filePath(QStringLiteral("history"));
    QDir{}.mkpath(dir);
    const QString dbPath = QDir{dir}.filePath(QStringLiteral("history-%1.db").arg(digest));
    auto opened = bootstrapper_->engine().openHistory(std::filesystem::path{dbPath.toStdString()});
    if (!opened) {
        emit notify(QStringLiteral("Could not open run history: %1")
                        .arg(QString::fromStdString(opened.error().detail)),
                    true);
    }
}

void AppController::refreshHistory() {
    if (!bootstrapper_) {
        return;
    }
    auto runs = bootstrapper_->engine().listRuns(100);
    if (!runs) {
        // History is best-effort UI; an unopened/unavailable store just
        // shows an empty list rather than surfacing an error to the user.
        history_.reset();
        return;
    }
    history_.reload(*runs);
}

void AppController::clearHistory() {
    if (!bootstrapper_) {
        return;
    }
    auto cleared = bootstrapper_->engine().clearHistory();
    if (!cleared) {
        emit notify(QString::fromStdString(cleared.error().detail), true);
        return;
    }
    refreshHistory();
    emit notify(QStringLiteral("Run history cleared"), false);
}

void AppController::replayRun(qulonglong runId) {
    if (!bootstrapper_) {
        return;
    }
    auto events = bootstrapper_->engine().historyEvents(engine::RunId{runId});
    if (!events) {
        emit notify(QString::fromStdString(events.error().detail), true);
        return;
    }

    // Rebuild the live timeline from the persisted events, reusing the same
    // formatting the live run path applies so a replayed run reads identically.
    timeline_.reset();
    const auto joinHeaders = [](const std::vector<std::pair<std::string, std::string>>& headers) {
        QString out;
        for (const auto& [key, value] : headers) {
            if (!out.isEmpty()) {
                out.append(QLatin1Char('\n'));
            }
            out.append(QString::fromStdString(key));
            out.append(QStringLiteral(": "));
            out.append(QString::fromStdString(value));
        }
        return out;
    };

    for (const auto& event : *events) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, engine::RunStarted>) {
                    timeline_.onRunStarted(QString::fromStdString(e.target.value),
                                           static_cast<int>(e.chainSize),
                                           QString::fromStdString(e.envName));
                } else if constexpr (std::is_same_v<T, engine::StepStarted>) {
                    timeline_.onStepStarted(static_cast<int>(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            e.attempt);
                } else if constexpr (std::is_same_v<T, engine::StepSkipped>) {
                    timeline_.onStepSkipped(static_cast<int>(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            format::skipReason(e.reason));
                } else if constexpr (std::is_same_v<T, engine::RequestPrepared>) {
                    timeline_.onRequestPrepared(static_cast<int>(e.stepIndex),
                                                format::method(e.method),
                                                QString::fromStdString(e.url),
                                                joinHeaders(e.maskedHeaders),
                                                static_cast<int>(e.bodySize));
                } else if constexpr (std::is_same_v<T, engine::ResponseReceived>) {
                    timeline_.onResponseReceived(
                        static_cast<int>(e.stepIndex),
                        e.status,
                        joinHeaders(e.headers),
                        static_cast<int>(e.bodySize),
                        static_cast<qint64>(e.elapsed.count()),
                        e.body ? QString::fromStdString(*e.body) : QString{});
                } else if constexpr (std::is_same_v<T, engine::ExtractionCompleted>) {
                    timeline_.onExtractionCompleted(static_cast<int>(e.stepIndex),
                                                    QString::fromStdString(e.op.value),
                                                    QString::fromStdString(e.variableName),
                                                    QString::fromStdString(e.sourcePath),
                                                    format::extractionOutcome(e.outcome),
                                                    QString::fromStdString(e.value));
                } else if constexpr (std::is_same_v<T, engine::AssertionCompleted>) {
                    timeline_.onAssertionCompleted(static_cast<int>(e.stepIndex),
                                                   QString::fromStdString(e.op.value),
                                                   QString::fromStdString(e.name),
                                                   QString::fromStdString(e.expr),
                                                   e.passed);
                } else if constexpr (std::is_same_v<T, engine::StepFailed>) {
                    timeline_.onStepFailed(static_cast<int>(e.stepIndex),
                                           QString::fromStdString(e.op.value),
                                           format::errorCode(e.code),
                                           QString::fromStdString(e.detail));
                } else if constexpr (std::is_same_v<T, engine::RunEnded>) {
                    timeline_.onRunEnded(format::runOutcome(e.outcome));
                }
            },
            event);
    }

    emit runReplayed();
}

QStringList AppController::operationIds() const {
    QStringList ids;
    if (activeProject().hasProject()) {
        for (const auto& [resId, resource] : activeProject().project().resources) {
            for (const auto& [opName, op] : resource.operations) {
                ids.append(QString::fromStdString(op.id.value));
            }
        }
    }
    return ids;
}

void AppController::selectOperationById(const QString& operationId) {
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return;
    }
    const QString module = operationId.left(dot);
    const QString opName = operationId.mid(dot + 1);
    if (selectedModule_ != module) {
        selectModule(module);
    }
    selectOperation(module, opName);
}

void AppController::editOperationById(const QString& operationId) {
    selectOperationById(operationId);
    if (hasOperation_ && !editing_) {
        beginEdit();
    }
}

void AppController::activateOperationById(const QString& operationId) {
    selectOperationById(operationId);
    if (hasOperation_) {
        runSelected(/*clean=*/false, /*dryRun=*/false);
    }
}

void AppController::setExplorerFilter(const QString& text) {
    treeFilter_.setFilterText(text);
}

bool AppController::isValidName(const QString& name) const {
    const QString trimmed = name.trimmed();
    return !trimmed.isEmpty() && !hasIdBreakingChars(trimmed);
}

void AppController::createResource(const QString& name, const QString& description) {
    QString error;
    if (activeProject().createResource(name, description, error)) {
        emit notify(QStringLiteral("Created module “%1”").arg(name.trimmed()), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::createProject(const QUrl& directory, const QString& name) {
    const QString path = directory.isLocalFile() ? directory.toLocalFile() : directory.toString();
    if (path.isEmpty()) {
        emit notify(tr("Choose a folder for the new project."), true);
        return;
    }
    QString error;
    if (activeProject().createProject(path, name, error)) {
        const QString shown = name.trimmed().isEmpty() ? tr("project") : name.trimmed();
        emit notify(QStringLiteral("Created project “%1”").arg(shown), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::prepareNewEndpoint(const QString& /*preselectedResource*/) {
    // Dependency candidates are every existing operation; the dialog filters
    // out self-reference by construction (the new op isn't created yet).
    newEndpointDeps_.setCandidates(operationIds());
    newEndpointExtractions_.clearRows();
    rebuildNewEndpointDepExtracts();
}

void AppController::rebuildNewEndpointDepExtracts() {
    std::vector<ChainEditorModel::OpSeed> seeds;
    if (activeProject().hasProject()) {
        for (const auto& depStd : newEndpointDeps_.dependencies()) {
            const QString id = QString::fromStdString(depStd);
            const auto* op = activeProject().findOperation(engine::OperationId{depStd});
            ChainEditorModel::OpSeed seed;
            seed.operationId = id;
            seed.method = op != nullptr ? methodLabel(op->method) : QString{};
            seed.isTarget = false;
            if (op != nullptr) {
                for (const auto& ext : op->extractions) {
                    seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                                  QString::fromStdString(ext.sourcePath));
                }
            }
            seeds.push_back(std::move(seed));
        }
    }
    newEndpointDepExtracts_.rebuild(seeds);
}

void AppController::addNewEndpointDependency(const QString& operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    // Setting the trailing blank (ghost) row appends and grows a new ghost.
    const int ghost = newEndpointDeps_.rowCount() - 1;
    newEndpointDeps_.setSelection(ghost >= 0 ? ghost : 0, operationId);
}

void AppController::removeNewEndpointDependency(const QString& operationId) {
    for (int row = 0; row < newEndpointDeps_.rowCount(); ++row) {
        const QString value =
            newEndpointDeps_.data(newEndpointDeps_.index(row, 0), DependencyEditModel::ValueRole)
                .toString();
        if (value == operationId) {
            newEndpointDeps_.removeRow(row);
            return;
        }
    }
}

void AppController::createOperation(const QString& module,
                                    const QString& name,
                                    const QString& method,
                                    const QString& path,
                                    const QString& actor) {
    std::vector<engine::OperationId> dependencies;
    for (const auto& dep : newEndpointDeps_.dependencies()) {
        dependencies.push_back(engine::OperationId{dep});
    }
    std::vector<engine::Extraction> extractions;
    for (const auto& [variable, sourcePath] : newEndpointExtractions_.pairs()) {
        if (sourcePath.isEmpty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = variable.toStdString();
        ext.sourcePath = sourcePath.toStdString();
        ext.source = sourceForPath(ext.sourcePath);
        extractions.push_back(std::move(ext));
    }

    QString error;
    const auto created = activeProject().createOperation(engine::ResourceId{module.toStdString()},
                                                         name,
                                                         methodFromLabel(method),
                                                         path,
                                                         engine::ActorId{actor.toStdString()},
                                                         dependencies,
                                                         extractions,
                                                         error);
    if (!created) {
        emit notify(error, true);
        return;
    }

    // Persist any edits to the dependencies' own extract blocks (the "pull
    // X from this prerequisite" rows), so a value declared here is saved on
    // the producing endpoint where the engine reads it.
    std::map<std::string, engine::Operation> depUpdates;
    for (int i = 0; i < newEndpointDepExtracts_.count(); ++i) {
        const QString id = newEndpointDepExtracts_.operationIdAt(i);
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        engine::Operation updated = *op;
        std::map<std::string, engine::Extraction::Source> sourceByVar;
        for (const auto& ext : op->extractions) {
            sourceByVar[ext.variableName] = ext.source;
        }
        updated.extractions.clear();
        for (const auto& [variable, sourcePath] :
             newEndpointDepExtracts_.extractModelAt(i)->pairs()) {
            const QString var = variable.trimmed();
            const QString p = sourcePath.trimmed();
            if (var.isEmpty() && p.isEmpty()) {
                continue;
            }
            engine::Extraction extraction;
            extraction.variableName = var.toStdString();
            extraction.sourcePath = p.toStdString();
            const auto found = sourceByVar.find(extraction.variableName);
            extraction.source =
                found != sourceByVar.end() ? found->second : sourceForPath(extraction.sourcePath);
            updated.extractions.push_back(std::move(extraction));
        }
        depUpdates.emplace(id.toStdString(), std::move(updated));
    }
    if (!depUpdates.empty()) {
        QString depError;
        if (!activeProject().saveOperations(depUpdates, depError)) {
            emit notify(depError, true);
        }
    }

    emit notify(QStringLiteral("Created endpoint “%1”").arg(QString::fromStdString(created->value)),
                false);
    selectOperationById(QString::fromStdString(created->value));
}

void AppController::renameOperation(const QString& operationId, const QString& newName) {
    QString error;
    if (activeProject().renameOperation(
            engine::OperationId{operationId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteOperation(const QString& operationId) {
    QString error;
    if (activeProject().deleteOperation(engine::OperationId{operationId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::renameResource(const QString& resourceId, const QString& newName) {
    QString error;
    if (activeProject().renameResource(
            engine::ResourceId{resourceId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed module"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteResource(const QString& resourceId) {
    QString error;
    if (activeProject().deleteResource(engine::ResourceId{resourceId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted module"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::selectExample(const QString& operationId, const QString& exampleName) {
    // Open the operation, then show the stored example response in the panel.
    selectOperationById(operationId);
    const QList<SavedResponse> saved = exampleStore_.list(operationId);
    for (const SavedResponse& r : saved) {
        if (r.name != exampleName) {
            continue;
        }
        hasResponse_ = true;
        shownExample_ = r.name;
        respStatus_ = r.status;
        respHeaders_ = r.headers;
        respBody_ = r.body;
        responseBody_.setBody(r.body);
        respBodySize_ = static_cast<int>(r.body.toUtf8().size());
        respElapsedMs_ = static_cast<int>(r.elapsedMs);
        runOutcome_.clear();
        emit responseChanged();
        return;
    }
    emit notify(QStringLiteral("Saved example not found"), true);
}

void AppController::renameExample(const QString& operationId,
                                  const QString& oldName,
                                  const QString& newName) {
    if (exampleStore_.rename(operationId, oldName, newName.trimmed())) {
        // If the renamed example is the one on screen, keep the label in sync.
        if (shownExample_ == oldName) {
            shownExample_ = newName.trimmed();
            emit responseChanged();
        }
        refreshExamples();
        emit notify(QStringLiteral("Renamed example to \u201C%1\u201D").arg(newName.trimmed()),
                    false);
    } else {
        emit notify(QStringLiteral("Couldn't rename example (empty or duplicate name)"), true);
    }
}

void AppController::duplicateExample(const QString& operationId, const QString& exampleName) {
    const QString created = exampleStore_.duplicate(operationId, exampleName);
    if (created.isEmpty()) {
        emit notify(QStringLiteral("Couldn't duplicate example"), true);
        return;
    }
    refreshExamples();
    emit notify(QStringLiteral("Duplicated as \u201C%1\u201D").arg(created), false);
}

void AppController::deleteExample(const QString& operationId, const QString& exampleName) {
    exampleStore_.remove(operationId, exampleName);
    refreshExamples();
    emit notify(QStringLiteral("Deleted example \u201C%1\u201D").arg(exampleName), false);
}

}  // namespace reqloom::desktop::qml
