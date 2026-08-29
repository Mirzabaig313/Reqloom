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
#include <QtCore/QVariantMap>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
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

[[nodiscard]] bool canDisplayTimelineIndex(const std::size_t index) noexcept {
    constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max() - 1);
    return index <= kMax;
}

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
    if (label == QStringLiteral("Bearer Token")) {
        return engine::AuthStrategy::Bearer;
    }
    if (label == QStringLiteral("JWT Bearer")) {
        return engine::AuthStrategy::Jwt;
    }
    if (label == QStringLiteral("mTLS (Client Cert)")) {
        return engine::AuthStrategy::Mtls;
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
    return {QStringLiteral("Multi-step login chain"),
            QStringLiteral("Single-step login"),
            QStringLiteral("Bearer Token"),
            QStringLiteral("OAuth 2.0 (Client Credentials)"),
            QStringLiteral("OAuth 2.0 (Password)"),
            QStringLiteral("API Key"),
            QStringLiteral("Basic Auth"),
            QStringLiteral("JWT Bearer"),
            QStringLiteral("mTLS (Client Cert)"),
            QStringLiteral("AWS Signature v4"),
            QStringLiteral("OAuth 1.0 (HMAC-SHA1)")};
}

QVariantMap AppController::actorConfigMap() const {
    QVariantMap out;
    for (const auto& [key, value] : actorConfig_.pairs()) {
        out.insert(key, value);
    }
    return out;
}

void AppController::setActorConfigValue(const QString& key, const QString& value) {
    auto pairs = actorConfig_.pairs();

    // Contract: an empty value clears the key. Drop it if present; if it was
    // absent, nothing changed. (Otherwise a cleared field would persist as an
    // empty config entry via saveActorInline instead of being removed.)
    if (value.isEmpty()) {
        if (std::erase_if(pairs, [&key](const auto& kv) { return kv.first == key; }) == 0) {
            return;
        }
        actorConfig_.setPairs(std::move(pairs));
        emit actorEditChanged();
        return;
    }

    bool found = false;
    for (auto& [existingKey, existingValue] : pairs) {
        if (existingKey == key) {
            if (existingValue == value) {
                return;  // no change — avoid re-emitting on every keystroke
            }
            existingValue = value;
            found = true;
            break;
        }
    }
    if (!found) {
        pairs.emplace_back(key, value);
    }
    actorConfig_.setPairs(std::move(pairs));
    emit actorEditChanged();
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

void AppController::newActor() {
    if (!activeProject().hasProject()) {
        emit notify(QStringLiteral("Open a project before creating an actor."), true);
        return;
    }
    // Open a fresh actor draft as its own tab (blank id → the inline panel
    // opens straight in edit mode).
    openActorTab(activeProject().rootPath(), QString{}, /*isNewDraft=*/true);
}

void AppController::requestActorEdit(const QString& projectRoot, const QString& actorId) {
    selectActor(projectRoot, actorId);
    if (hasActor_) {
        emit actorEditRequested();
    }
}

void AppController::cancelActorDraft() {
    // Cancelling a never-saved draft closes its tab.
    if (tabModel()->valid(activeTabIndex())) {
        closeTab(activeTabIndex());
    }
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
    // Point the read-only selection at the (about-to-exist) actor so onSaved —
    // which runs synchronously inside saveActor on success — re-seeds it (covers
    // create with an empty originalId, and rename). Restore on failure so a
    // rejected save leaves the draft identity intact (an empty name still reads
    // as a "New actor" draft in the panel).
    const QString savedName = name.trimmed();
    const QString prevId = selectedActorId_;
    const QString prevName = selectedActorName_;
    selectedActorId_ = savedName;
    selectedActorName_ = savedName;
    if (activeProject().saveActor(originalId, actor, error)) {
        // Point the active actor tab at the saved id (a draft's blank id
        // becomes the real name) so switching away/back finds it.
        if (tabModel()->valid(activeTabIndex()) &&
            tabModel()->stateAt(activeTabIndex()).kind == TabState::Kind::Actor) {
            TabState& tab = tabModel()->stateAt(activeTabIndex());
            tab.id = savedName;
            tab.title = savedName;
            tab.dirty = false;  // just saved — clear the unsaved dot
            tabModel()->refreshRow(activeTabIndex());
            persistOpenTabs();  // a draft's blank id is now reopenable
        }
        emit notify(QStringLiteral("Saved actor “%1”").arg(savedName), false);
        return true;
    }
    selectedActorId_ = prevId;
    selectedActorName_ = prevName;
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
                if constexpr (requires { e.stepIndex; }) {
                    if (!canDisplayTimelineIndex(e.stepIndex)) {
                        return;
                    }
                }
                if constexpr (std::is_same_v<T, engine::StepBlocked>) {
                    if (!canDisplayTimelineIndex(e.blockedByStepIndex)) {
                        return;
                    }
                }
                if constexpr (std::is_same_v<T, engine::RunStarted>) {
                    timeline_.onRunStarted(QString::fromStdString(e.target.value),
                                           static_cast<int>(e.chainSize),
                                           QString::fromStdString(e.envName));
                } else if constexpr (std::is_same_v<T, engine::StepStarted>) {
                    timeline_.onStepStarted(format::boundedIndex(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            e.attempt);
                } else if constexpr (std::is_same_v<T, engine::StepSkipped>) {
                    timeline_.onStepSkipped(format::boundedIndex(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            format::skipReason(e.reason));
                } else if constexpr (std::is_same_v<T, engine::RequestPrepared>) {
                    timeline_.onRequestPrepared(format::boundedIndex(e.stepIndex),
                                                format::method(e.method),
                                                QString::fromStdString(e.url),
                                                joinHeaders(e.maskedHeaders),
                                                static_cast<int>(e.bodySize));
                } else if constexpr (std::is_same_v<T, engine::ResponseReceived>) {
                    timeline_.onResponseReceived(
                        format::boundedIndex(e.stepIndex),
                        e.status,
                        joinHeaders(e.headers),
                        static_cast<int>(e.bodySize),
                        static_cast<qint64>(e.elapsed.count()),
                        e.body ? QString::fromStdString(*e.body) : QString{});
                } else if constexpr (std::is_same_v<T, engine::ExtractionCompleted>) {
                    timeline_.onExtractionCompleted(format::boundedIndex(e.stepIndex),
                                                    QString::fromStdString(e.op.value),
                                                    QString::fromStdString(e.variableName),
                                                    QString::fromStdString(e.sourcePath),
                                                    format::extractionOutcome(e.outcome),
                                                    QString::fromStdString(e.value));
                } else if constexpr (std::is_same_v<T, engine::AssertionCompleted>) {
                    timeline_.onAssertionCompleted(format::boundedIndex(e.stepIndex),
                                                   QString::fromStdString(e.op.value),
                                                   QString::fromStdString(e.name),
                                                   QString::fromStdString(e.expr),
                                                   e.passed);
                } else if constexpr (std::is_same_v<T, engine::StepFailed>) {
                    timeline_.onStepFailed(format::boundedIndex(e.stepIndex),
                                           QString::fromStdString(e.op.value),
                                           format::errorCode(e.code),
                                           QString::fromStdString(e.detail),
                                           format::unresolvedDiagnostics(e.diagnostics));
                } else if constexpr (std::is_same_v<T, engine::StepBlocked>) {
                    timeline_.onStepBlocked(format::boundedIndex(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            format::boundedIndex(e.blockedByStepIndex));
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

QString AppController::operationMethod(const QString& operationId) const {
    const auto* op = activeProject().findOperation(engine::OperationId{operationId.toStdString()});
    return op != nullptr ? methodLabel(op->method) : QString{};
}

void AppController::selectOperationById(const QString& operationId) {
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return;
    }
    const QString module = operationId.left(dot);
    const QString opName = operationId.mid(dot + 1);
    // openOperationTab (via selectOperation) sets the module context itself;
    // no separate selectModule (which would blank the active tab).
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
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before creating another project."), true);
        return;
    }
    if (!canLeaveActiveProject()) {
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

void AppController::prepareNewEndpoint(const QString& preselectedResource) {
    if (!activeProject().hasProject()) {
        return;
    }
    if (const int draftIndex = operationDraftIndex(); draftIndex >= 0) {
        activateTab(draftIndex);
        return;
    }
    QString module = preselectedResource.trimmed();
    const auto& projectResources = activeProject().project().resources;
    if (module.isEmpty() &&
        projectResources.contains(engine::ResourceId{selectedModule_.toStdString()})) {
        module = selectedModule_;
    }
    if (module.isEmpty() && projectResources.size() == 1) {
        module = QString::fromStdString(projectResources.begin()->first.value);
    }
    if (!projectResources.contains(engine::ResourceId{module.toStdString()})) {
        emit notify(tr("Select a module in the Explorer before creating an endpoint."), true);
        return;
    }

    captureActiveTab();
    creatingOperation_ = true;
    newOperationModule_ = module;
    newOperationDraftId_ = module + QStringLiteral(".__new_endpoint__");
    for (int suffix = 2; operationIds().contains(newOperationDraftId_); ++suffix) {
        newOperationDraftId_ = module + QStringLiteral(".__new_endpoint__%1").arg(suffix);
    }
    newOperationName_.clear();

    // A default TabState is the canonical empty edit buffer. Reusing its
    // restore path resets every request/auth/body/option/assertion field.
    restoreOperationEditFrom(TabState{});
    editMethod_ = QStringLiteral("GET");
    editing_ = true;
    chainFieldsLoaded_ = true;
    hasActor_ = false;
    hasOperation_ = true;
    selectedModule_ = module;
    opName_.clear();
    opMethod_ = editMethod_;
    opPath_.clear();
    opActor_.clear();
    opBody_.clear();
    opDependencies_.clear();
    opHeaders_.reset();
    opQuery_.reset();
    opExtractions_.reset();
    opAssertions_.reset();
    exampleList_.clear();
    timeline_.reset();

    ChainEditorModel::OpSeed target;
    target.operationId = newOperationDraftId_;
    target.method = editMethod_;
    target.isTarget = true;
    target.candidates = operationIds();
    chainEditor_.rebuild({std::move(target)});

    TabState tab;
    tab.kind = TabState::Kind::Operation;
    tab.id = newOperationDraftId_;
    tab.module = module;
    tab.projectRoot = activeProject().rootPath();
    tab.title = tr("New endpoint");
    tab.method = editMethod_;
    tab.dirty = true;
    tab.operationDraft = true;
    const int insertAt = activeTabIndex_ >= 0 ? activeTabIndex_ + 1 : tabs_.count();
    activeTabIndex_ = tabs_.insert(insertAt, std::move(tab));
    captureActiveTab();

    emit operationChanged();
    emit actorSelectionChanged();
    emit editingChanged();
    emit editChanged();
    emit responseChanged();
    emit chainChanged();
    emit selectionChanged();
    emit activeTabChanged();
    persistOpenTabs();
}

void AppController::saveNewOperation() {
    if (!creatingOperation_ || !activeProject().hasProject()) {
        return;
    }

    engine::Operation operation;
    applyOverrideToOperation(operation, buildOverride());
    const QString targetId = currentOperationId();
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) != targetId) {
            continue;
        }
        const QString overResource = chainEditor_.forEachOverAt(i).trimmed();
        if (!overResource.isEmpty()) {
            engine::ForEach forEach{engine::ResourceId{overResource.toStdString()}};
            forEach.continueOnError = chainEditor_.forEachContinueOnErrorAt(i);
            operation.forEach = std::move(forEach);
        }
        break;
    }

    const QString targetModule = newOperationModule_;
    const QString targetName = newOperationName_;
    QString error;
    const auto created = activeProject().createOperation(
        engine::ResourceId{targetModule.toStdString()}, targetName, operation, error);
    if (!created) {
        emit notify(error, true);
        return;
    }

    const QString createdId = QString::fromStdString(created->value);
    const QString createdName = targetName.trimmed();
    captureActiveTab();
    const int draftIndex = operationDraftIndex();
    if (draftIndex < 0) {
        clearNewOperationDraftIdentity();
        keepEditingNewOperation();
        openOperationTab(activeProject().rootPath(), targetModule, createdName);
        emit notify(QStringLiteral("Created endpoint “%1”").arg(createdId), false);
        return;
    }

    const bool draftIsActive = draftIndex == activeTabIndex_;
    TabState& tab = tabs_.stateAt(draftIndex);
    tab.id = createdId;
    tab.opName = createdName;
    tab.title = createdName;
    tab.operationDraft = false;
    tab.editing = false;
    tab.chainFieldsLoaded = false;
    tab.chainSnapshotValid = false;
    tab.dirty = false;
    clearNewOperationDraftIdentity();
    keepEditingNewOperation();
    if (draftIsActive) {
        restoreActiveTab();
    }
    tabs_.refreshRow(draftIndex);
    emit activeTabChanged();
    persistOpenTabs();
    emit notify(QStringLiteral("Created endpoint “%1”").arg(createdId), false);
}

void AppController::clearNewOperationDraftIdentity() {
    creatingOperation_ = false;
    newOperationModule_.clear();
    newOperationDraftId_.clear();
    newOperationName_.clear();
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
