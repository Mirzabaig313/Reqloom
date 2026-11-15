// TabEdits — see header.
#include "TabEdits.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLatin1Char>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] QString tr(const char* text, int n = -1) {
    return QCoreApplication::translate("TabEdits", text, nullptr, n);
}

[[nodiscard]] auto requestFields(const TabState& t) {
    return std::tie(t.editMethod,
                    t.editPath,
                    t.editActor,
                    t.editExpectStatus,
                    t.editTimeout,
                    t.editForce,
                    t.editBody,
                    t.editBodyIsForm,
                    t.editBodyType,
                    t.editHeaders,
                    t.editForm,
                    t.editExtractions,
                    t.editAssertions,
                    t.editDependencies);
}

[[nodiscard]] auto authFields(const TabState& t) {
    return std::tie(t.editAuthType,
                    t.editAuthToken,
                    t.editAuthUsername,
                    t.editAuthPassword,
                    t.editAuthApiKeyName,
                    t.editAuthApiKeyValue,
                    t.editAuthApiKeyInQuery,
                    t.editAuthAwsAccessKey,
                    t.editAuthAwsSecretKey,
                    t.editAuthAwsRegion,
                    t.editAuthAwsService,
                    t.editAuthAwsSessionToken,
                    t.editAuthOauthConsumerKey,
                    t.editAuthOauthConsumerSecret,
                    t.editAuthOauthToken,
                    t.editAuthOauthTokenSecret,
                    t.editAuthOauth2GrantType,
                    t.editAuthOauth2ClientAuth,
                    t.editAuthOauth2AuthUrl,
                    t.editAuthOauth2CallbackUrl,
                    t.editAuthOauth2PkceMethod,
                    t.editAuthOauth2TokenUrl,
                    t.editAuthOauth2ClientId,
                    t.editAuthOauth2ClientSecret,
                    t.editAuthOauth2Scope,
                    t.editAuthJwtAlgorithm,
                    t.editAuthJwtSecret,
                    t.editAuthJwtPayload,
                    t.editAuthMtlsFormat,
                    t.editAuthMtlsCertPath,
                    t.editAuthMtlsKeyPath,
                    t.editAuthMtlsKeyPassword,
                    t.editAuthMtlsCaCertPath);
}

[[nodiscard]] bool sameSeed(const ChainEditorModel::OpSeed& a, const ChainEditorModel::OpSeed& b) {
    return std::tie(a.operationId,
                    a.dependencies,
                    a.extractions,
                    a.forEachOver,
                    a.forEachContinueOnError) == std::tie(b.operationId,
                                                          b.dependencies,
                                                          b.extractions,
                                                          b.forEachOver,
                                                          b.forEachContinueOnError);
}

[[nodiscard]] bool sameChain(const TabState& a, const TabState& b) {
    static const std::vector<ChainEditorModel::OpSeed> kNone;
    const auto& left = a.chainSnapshotValid ? a.chainSeeds : kNone;
    const auto& right = b.chainSnapshotValid ? b.chainSeeds : kNone;
    return std::ranges::equal(left, right, sameSeed);
}

[[nodiscard]] bool sameStep(const AuthStepListModel::StepSeed& a,
                            const AuthStepListModel::StepSeed& b) {
    return std::tie(a.id, a.method, a.path, a.body, a.expect, a.extractions) ==
           std::tie(b.id, b.method, b.path, b.body, b.expect, b.extractions);
}

[[nodiscard]] TabKvPairs sortedPairs(TabKvPairs pairs) {
    std::ranges::sort(pairs);
    return pairs;
}

bool replaceIn(std::vector<QString>& ids, const QString& from, const QString& to) {
    bool changed = false;
    for (QString& id : ids) {
        if (id == from) {
            id = to;
            changed = true;
        }
    }
    return changed;
}

bool replaceIn(std::vector<std::string>& ids, const std::string& from, const std::string& to) {
    bool changed = false;
    for (std::string& id : ids) {
        if (id == from) {
            id = to;
            changed = true;
        }
    }
    return changed;
}

bool renameInSeeds(std::vector<ChainEditorModel::OpSeed>& seeds,
                   const QString& oldId,
                   const QString& newId) {
    bool changed = false;
    for (auto& seed : seeds) {
        if (seed.operationId == oldId) {
            seed.operationId = newId;
            changed = true;
        }
        changed = replaceIn(seed.dependencies, oldId.toStdString(), newId.toStdString()) || changed;
    }
    return changed;
}

bool forgetInSeeds(std::vector<ChainEditorModel::OpSeed>& seeds, const QString& operationId) {
    const std::string id = operationId.toStdString();
    bool changed = false;
    for (auto& seed : seeds) {
        changed = std::erase(seed.dependencies, id) > 0 || changed;
    }
    return changed;
}

// Applies `edit` to the tab's live edits and to a private copy of its baseline (baselines are
// shared and immutable).
template <typename Edit>
bool editLiveAndBaseline(TabState& tab, Edit edit) {
    bool changed = edit(tab);
    if (tab.editBaseline) {
        auto baseline = std::make_shared<TabState>(*tab.editBaseline);
        if (edit(*baseline)) {
            tab.editBaseline = std::move(baseline);
            changed = true;
        }
    }
    return changed;
}

[[nodiscard]] QString moduleOf(const QString& operationId) {
    return operationId.section(QLatin1Char('.'), 0, 0);
}

}  // namespace

bool sameOperationEdits(const TabState& a, const TabState& b) {
    return requestFields(a) == requestFields(b) && authFields(a) == authFields(b) &&
           sameChain(a, b);
}

bool sameActorEdits(const TabState& a, const TabState& b) {
    const auto refresh = [](const TabState& t) {
        const bool on = t.actorEditRefreshEnabled;
        return std::make_tuple(on,
                               on ? t.actorEditRefreshMethod : QString{},
                               on ? t.actorEditRefreshPath : QString{},
                               on ? t.actorEditRefreshBody : QString{},
                               on ? t.actorRefreshExtract : TabKvPairs{});
    };
    return a.actorEditName.trimmed() == b.actorEditName.trimmed() &&
           a.actorEditDescription.trimmed() == b.actorEditDescription.trimmed() &&
           a.actorEditStrategy == b.actorEditStrategy &&
           sortedPairs(a.actorConfig) == sortedPairs(b.actorConfig) &&
           std::ranges::equal(a.actorAuthSteps, b.actorAuthSteps, sameStep) &&
           refresh(a) == refresh(b);
}

bool hasUnsavedEdits(const TabState& tab) {
    if (tab.kind == TabState::Kind::Actor) {
        return tab.id.isEmpty() ||
               (tab.actorEditing && tab.editBaseline && !sameActorEdits(tab, *tab.editBaseline));
    }
    if (tab.operationDraft) {
        if (!tab.opName.trimmed().isEmpty() || !tab.editBaseline) {
            return true;
        }
        return !sameOperationEdits(tab, *tab.editBaseline);
    }
    return tab.editing && tab.editBaseline && !sameOperationEdits(tab, *tab.editBaseline);
}

bool renameOperationInTab(TabState& tab, const QString& oldId, const QString& newId) {
    if (tab.kind != TabState::Kind::Operation || oldId == newId) {
        return false;
    }
    bool changed = false;
    const QString oldModule = moduleOf(oldId);
    const QString newModule = moduleOf(newId);
    if (!tab.operationDraft && tab.id == oldId) {
        tab.id = newId;
        tab.module = newModule;
        tab.opName = newId.mid(newModule.size() + 1);
        tab.title = tab.opName;
        changed = true;
    } else if (tab.operationDraft && oldModule != newModule && tab.module == oldModule) {
        tab.id = newModule + tab.id.mid(oldModule.size());
        tab.module = newModule;
        changed = true;
    }
    return editLiveAndBaseline(tab,
                               [&oldId, &newId](TabState& t) {
                                   const bool deps = replaceIn(t.editDependencies, oldId, newId);
                                   return renameInSeeds(t.chainSeeds, oldId, newId) || deps;
                               }) ||
           changed;
}

bool forgetOperationInTab(TabState& tab, const QString& operationId) {
    if (tab.kind != TabState::Kind::Operation) {
        return false;
    }
    return editLiveAndBaseline(tab, [&operationId](TabState& t) {
        const bool deps = std::erase(t.editDependencies, operationId) > 0;
        return forgetInSeeds(t.chainSeeds, operationId) || deps;
    });
}

void detachDeletedOperation(TabState& tab, const QString& draftId) {
    renameInSeeds(tab.chainSeeds, tab.id, draftId);
    tab.id = draftId;
    tab.operationDraft = true;
    tab.editing = true;
    tab.chainFieldsLoaded = true;
    // Compared against a blank draft, so the kept edits stay dirty even once the name is cleared.
    tab.editBaseline = std::make_shared<const TabState>();
    tab.dirty = true;
}

TabRestorePlan planTabRestore(const std::vector<SavedTab>& saved,
                              const std::function<bool(const SavedTab&)>& canReopen) {
    TabRestorePlan plan;
    for (const SavedTab& tab : saved) {
        if (canReopen(tab)) {
            plan.reopen.push_back(tab);
        } else {
            ++plan.dropped;
        }
    }
    return plan;
}

QString droppedTabsMessage(int dropped) {
    return dropped == 1 ? tr("1 saved tab could not be reopened because its endpoint or actor no "
                             "longer exists.")
                        : tr("%1 saved tabs could not be reopened because their endpoints or "
                             "actors no longer exist.")
                              .arg(dropped);
}

UnsavedPrompt unsavedPrompt(const QStringList& titles, UnsavedReason reason, bool cancelsRun) {
    const bool one = titles.size() == 1;
    UnsavedPrompt prompt;
    prompt.title = tr("Unsaved changes");
    QString question;
    switch (reason) {
        case UnsavedReason::CloseTabs:
            question = one ? tr("Save them before closing the tab?")
                           : tr("Save them before closing the tabs?");
            break;
        case UnsavedReason::LeaveProject:
            question = tr("Save them before the project's tabs close?");
            break;
        case UnsavedReason::Quit:
            question = tr("Save them before quitting?");
            break;
        case UnsavedReason::CloseEnvironment:
            question = tr("Save them before closing?");
            break;
        case UnsavedReason::SwitchEnvironment:
            question = tr("Save them before opening another environment?");
            break;
    }
    const QString subject =
        one ? tr("“%1” has unsaved changes.").arg(titles.front())
            : tr("%n tabs have unsaved changes: %1.", static_cast<int>(titles.size()))
                  .arg(titles.join(QStringLiteral(", ")));
    prompt.message = subject + QLatin1Char(' ') + question;
    if (cancelsRun) {
        prompt.message += QLatin1Char(' ') + tr("Closing also cancels the run in progress.");
    }
    prompt.saveText = one ? tr("Save") : tr("Save all");
    prompt.discardText = one ? tr("Discard") : tr("Discard all");
    prompt.cancelText = tr("Cancel");
    return prompt;
}

bool saveInOrder(const std::vector<std::uint64_t>& uids,
                 const std::function<bool(std::uint64_t)>& save) {
    return std::ranges::all_of(uids, save);
}

const QHash<QString, QString>& shownChainStatus(const TabState* active,
                                                bool activeOwnsRun,
                                                bool runHasTab,
                                                const QHash<QString, QString>& live) {
    static const QHash<QString, QString> kNone;
    if (active == nullptr) {
        return runHasTab ? kNone : live;
    }
    return activeOwnsRun ? live : active->chainStatus;
}

}  // namespace reqloom::desktop::qml
