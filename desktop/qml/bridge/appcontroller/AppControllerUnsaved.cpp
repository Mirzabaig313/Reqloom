// AppController — unsaved edits per tab: dirty tracking against each tab's edit baseline, actor
// edit mode, the Save / Discard / Cancel prompt before edits are dropped (tabs and the environment
// editor), and keeping tabs pointed at operations that are renamed or deleted.
#include "AppController.h"

#include <QtCore/QMetaObject>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <array>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] std::vector<int> allIndexes(int count) {
    std::vector<int> out(static_cast<std::size_t>(count));
    std::iota(out.begin(), out.end(), 0);
    return out;
}

}  // namespace

void AppController::wireTabTracking() {
    const auto countChanged = [this]() {
        emit unsavedTabsChanged();
    };
    connect(&tabs_, &QAbstractItemModel::dataChanged, this, countChanged);
    connect(&tabs_, &QAbstractItemModel::rowsInserted, this, countChanged);
    connect(&tabs_, &QAbstractItemModel::rowsRemoved, this, countChanged);
    connect(&tabs_, &QAbstractItemModel::modelReset, this, countChanged);

    connect(this, &AppController::editingChanged, this, &AppController::onActiveTabEditingChanged);
    const auto edited = [this]() {
        scheduleActiveTabDirtyUpdate();
    };
    connect(this, &AppController::editChanged, this, edited);
    connect(this, &AppController::actorEditChanged, this, edited);
    connect(this, &AppController::actorEditFieldsChanged, this, edited);
    connect(&actorAuthSteps_, &AuthStepListModel::stepsEdited, this, edited);
    connect(&actorRefreshExtract_, &QAbstractItemModel::dataChanged, this, edited);
    connect(&actorRefreshExtract_, &QAbstractItemModel::rowsInserted, this, edited);
    connect(&actorRefreshExtract_, &QAbstractItemModel::rowsRemoved, this, edited);

    connect(this, &AppController::operationRenamed, this, &AppController::renameOperationInTabs);
    connect(this, &AppController::operationRemoved, this, &AppController::removeOperationFromTabs);

    // The run writes the live map; each change is kept by the tab that started the run, so the
    // graph of another tab shows that tab's own last run.
    connect(this, &AppController::chainStatusChanged, this, [this]() {
        if (const int index = runTabIndex(); index >= 0) {
            tabs_.stateAt(index).chainStatus = chainStatus_;
        }
    });
    connect(this, &AppController::activeTabChanged, this, &AppController::chainStatusChanged);
}

TabState AppController::liveOperationEdits() const {
    TabState live;
    captureOperationInto(live);
    if (chainFieldsLoaded_ && chainEditor_.count() > 0) {
        live.chainSeeds = chainEditor_.snapshotSeeds();
        live.chainSnapshotValid = true;
    }
    if (tabs_.valid(activeTabIndex_)) {
        const TabState& tab = tabs_.stateAt(activeTabIndex_);
        live.id = tab.id;
        live.operationDraft = tab.operationDraft;
        live.editBaseline = tab.editBaseline;
    }
    live.opName = creatingOperation_ ? newOperationName_ : opName_;
    return live;
}

void AppController::scheduleActiveTabDirtyUpdate() {
    // Deferred: mid-switch, the live fields briefly belong to neither tab.
    if (dirtyUpdatePending_) {
        return;
    }
    dirtyUpdatePending_ = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            dirtyUpdatePending_ = false;
            updateActiveTabDirty();
        },
        Qt::QueuedConnection);
}

void AppController::updateActiveTabDirty() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    bool dirty = false;
    if (tab.kind == TabState::Kind::Operation) {
        dirty = hasUnsavedEdits(liveOperationEdits());
    } else {
        TabState live;
        live.kind = TabState::Kind::Actor;
        live.id = tab.id;
        live.editBaseline = tab.editBaseline;
        captureActorInto(live);
        dirty = hasUnsavedEdits(live);
    }
    if (tab.dirty != dirty) {
        tab.dirty = dirty;
        tabs_.refreshRow(activeTabIndex_);
    }
}

void AppController::onActiveTabEditingChanged() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    if (tab.kind != TabState::Kind::Operation) {
        return;
    }
    if (!editing_) {
        tab.editBaseline.reset();
    } else if (!tab.editBaseline) {
        TabState baseline = liveOperationEdits();
        baseline.editBaseline.reset();
        tab.editBaseline = std::make_shared<const TabState>(std::move(baseline));
    }
    updateActiveTabDirty();
}

void AppController::rebaseChainEdits() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    if (!tab.editBaseline) {
        return;
    }
    const TabState live = liveOperationEdits();
    auto baseline = std::make_shared<TabState>(*tab.editBaseline);
    baseline->editDependencies = live.editDependencies;
    baseline->editExtractions = live.editExtractions;
    baseline->editAssertions = live.editAssertions;
    baseline->chainSeeds = live.chainSeeds;
    baseline->chainSnapshotValid = live.chainSnapshotValid;
    tab.editBaseline = std::move(baseline);
    updateActiveTabDirty();
}

bool AppController::askAboutUnsaved(const std::vector<int>& closing,
                                    UnsavedReason reason,
                                    std::function<void()> proceed) {
    if (pendingUnsaved_) {
        return true;
    }
    captureActiveTab();
    PendingUnsaved pending;
    QStringList titles;
    bool cancelsRun = false;
    for (const int index : closing) {
        if (!tabs_.valid(index)) {
            continue;
        }
        const TabState& tab = tabs_.stateAt(index);
        cancelsRun = cancelsRun || runRouter_.originIs({static_cast<int>(tab.kind), tab.id});
        if (tab.dirty) {
            pending.uids.push_back(tab.uid);
            titles.append(tabs_.displayTitle(index));
        }
    }
    if (pending.uids.empty()) {
        return false;
    }
    pending.proceed = std::move(proceed);
    pendingUnsaved_ = std::move(pending);
    const UnsavedPrompt prompt =
        unsavedPrompt(titles, reason, cancelsRun && reason == UnsavedReason::CloseTabs);
    emit unsavedChangesPrompt(
        prompt.title, prompt.message, prompt.saveText, prompt.discardText, prompt.cancelText);
    return true;
}

void AppController::resolveUnsavedChanges(int choice) {
    if (!pendingUnsaved_) {
        return;
    }
    PendingUnsaved pending = std::move(*pendingUnsaved_);
    pendingUnsaved_.reset();
    if (choice != 0 && choice != 1) {
        return;
    }
    const auto saveTab = [this](std::uint64_t uid) {
        const int index = tabs_.indexOfUid(uid);
        return index < 0 || saveTabAt(index);
    };
    if (choice == 0 && !saveInOrder(pending.uids, saveTab)) {
        return;
    }
    pending.proceed();
}

bool AppController::saveTabAt(int index) {
    activateTab(index);
    captureActiveTab();
    const TabState& tab = tabs_.stateAt(activeTabIndex_);
    if (!tab.dirty) {
        return true;
    }
    if (tab.kind == TabState::Kind::Actor) {
        saveActorEdits();
    } else {
        saveOperation();
    }
    captureActiveTab();
    return !tabs_.stateAt(activeTabIndex_).dirty;
}

template <typename T>
void AppController::setActorEditField(T& field, const T& value) {
    if (field == value) {
        return;
    }
    field = value;
    emit actorEditFieldsChanged();
}

void AppController::setActorEditName(const QString& value) {
    setActorEditField(actorEditName_, value);
}

void AppController::setActorEditDescription(const QString& value) {
    setActorEditField(actorEditDescription_, value);
}

void AppController::setActorEditStrategy(const QString& value) {
    setActorEditField(actorEditStrategy_, value);
}

void AppController::setActorEditRefreshEnabled(bool value) {
    setActorEditField(actorEditRefreshEnabled_, value);
}

void AppController::setActorEditRefreshMethod(const QString& value) {
    setActorEditField(actorEditRefreshMethod_, value);
}

void AppController::setActorEditRefreshPath(const QString& value) {
    setActorEditField(actorEditRefreshPath_, value);
}

void AppController::setActorEditRefreshBody(const QString& value) {
    setActorEditField(actorEditRefreshBody_, value);
}

void AppController::seedActorEditFields() {
    actorEditName_ = selectedActorName_;
    actorEditDescription_ = selectedActorDescription_;
    const QStringList strategies = actorStrategies();
    actorEditStrategy_ =
        strategies.contains(selectedActorStrategy_) ? selectedActorStrategy_ : strategies.front();
    actorEditRefreshEnabled_ = actorHasRefresh_;
    actorEditRefreshMethod_ = actorRefreshMethod_;
    actorEditRefreshPath_ = actorRefreshPath_;
    actorEditRefreshBody_ = actorRefreshBody_;
}

void AppController::beginActorEdit() {
    if (actorEditing_ || !tabs_.valid(activeTabIndex_) ||
        tabs_.stateAt(activeTabIndex_).kind != TabState::Kind::Actor) {
        return;
    }
    seedActorEditFields();
    actorEditing_ = true;
    TabState baseline;
    baseline.kind = TabState::Kind::Actor;
    captureActorInto(baseline);
    tabs_.stateAt(activeTabIndex_).editBaseline =
        std::make_shared<const TabState>(std::move(baseline));
    emit actorEditFieldsChanged();
    updateActiveTabDirty();
}

void AppController::endActorEdit() {
    actorEditing_ = false;
    if (tabs_.valid(activeTabIndex_) &&
        tabs_.stateAt(activeTabIndex_).kind == TabState::Kind::Actor) {
        tabs_.stateAt(activeTabIndex_).editBaseline.reset();
    }
    emit actorEditFieldsChanged();
    updateActiveTabDirty();
}

void AppController::discardActorEdits() {
    if (!tabs_.valid(activeTabIndex_) ||
        tabs_.stateAt(activeTabIndex_).kind != TabState::Kind::Actor) {
        return;
    }
    if (selectedActorId_.isEmpty()) {
        closeTabImmediately(activeTabIndex_);
        return;
    }
    prepareEditActor(selectedActorId_);
    endActorEdit();
}

bool AppController::saveActorEdits() {
    if (!actorEditing_) {
        return false;
    }
    // Out of edit mode first, so the save's own refresh re-seeds the panel from the saved actor.
    actorEditing_ = false;
    if (!saveActorInline(selectedActorId_,
                         actorEditName_.trimmed(),
                         actorEditStrategy_,
                         actorEditDescription_.trimmed(),
                         actorEditRefreshEnabled_,
                         actorEditRefreshMethod_,
                         actorEditRefreshPath_,
                         actorEditRefreshBody_)) {
        actorEditing_ = true;
        return false;
    }
    endActorEdit();
    return true;
}

bool AppController::environmentEditsChanged(const QString& name) const {
    return hasUnsavedEnvironmentEdits(envBaseline_, {name, editEnvBaseUrl_, envVars_.pairs()});
}

QVariantMap AppController::environmentUnsavedPrompt(const QString& title, int reason) const {
    static constexpr std::array kReasons{
        UnsavedReason::CloseEnvironment, UnsavedReason::SwitchEnvironment, UnsavedReason::Quit};
    const bool known = reason >= 0 && reason < static_cast<int>(kReasons.size());
    const UnsavedPrompt prompt = unsavedPrompt(
        {title}, known ? kReasons.at(static_cast<std::size_t>(reason)) : kReasons.front(), false);
    return {{QStringLiteral("title"), prompt.title},
            {QStringLiteral("message"), prompt.message},
            {QStringLiteral("saveText"), prompt.saveText},
            {QStringLiteral("discardText"), prompt.discardText},
            {QStringLiteral("cancelText"), prompt.cancelText}};
}

bool AppController::requestQuit() {
    return !askAboutUnsaved(
        allIndexes(tabs_.count()), UnsavedReason::Quit, [this]() { emit quitApproved(); });
}

bool AppController::canLeaveActiveProject(const std::function<void()>& retry) {
    if (leaveApproved_) {
        return true;
    }
    return !askAboutUnsaved(
        allIndexes(tabs_.count()), UnsavedReason::LeaveProject, [this, retry]() {
            const QScopedValueRollback<bool> approved{leaveApproved_, true};
            retry();
        });
}

void AppController::renameOperationInTabs(const QString& oldId, const QString& newId) {
    captureActiveTab();
    const int operationKind = static_cast<int>(TabState::Kind::Operation);
    runRouter_.renameOrigin({operationKind, oldId}, {operationKind, newId});
    bool activeChanged = false;
    for (int i = 0; i < tabs_.count(); ++i) {
        if (renameOperationInTab(tabs_.stateAt(i), oldId, newId)) {
            tabs_.refreshRow(i);
            activeChanged = activeChanged || i == activeTabIndex_;
        }
    }
    if (!activeChanged || tabs_.stateAt(activeTabIndex_).kind != TabState::Kind::Operation) {
        persistOpenTabs();
        return;
    }
    // Point the live editor at the new id so Save and Run address the renamed operation.
    const TabState& tab = tabs_.stateAt(activeTabIndex_);
    if (tab.operationDraft) {
        newOperationModule_ = tab.module;
        newOperationDraftId_ = tab.id;
    } else {
        opName_ = tab.opName;
    }
    selectedModule_ = tab.module;
    restoreOperationEditFrom(tab);
    if (editing_ && tab.chainSnapshotValid) {
        chainEditor_.rebuild(tab.chainSeeds);
    }
    persistOpenTabs();
    emit operationChanged();
    emit editingChanged();
    emit editChanged();
    emit chainChanged();
    emit selectionChanged();
    emit activeTabChanged();
}

void AppController::removeOperationFromTabs(const QString& operationId) {
    captureActiveTab();
    for (int i = 0; i < tabs_.count(); ++i) {
        if (forgetOperationInTab(tabs_.stateAt(i), operationId) && i == activeTabIndex_) {
            restoreOperationEditFrom(tabs_.stateAt(i));
            if (editing_ && tabs_.stateAt(i).chainSnapshotValid) {
                chainEditor_.rebuild(tabs_.stateAt(i).chainSeeds);
            }
            emit editChanged();
        }
    }
    const int index = tabs_.indexOf(TabState::Kind::Operation, operationId);
    if (index < 0 || tabs_.stateAt(index).operationDraft) {
        return;
    }
    if (!tabs_.stateAt(index).dirty) {
        closeTabImmediately(index);
        return;
    }
    detachTab(index);
}

void AppController::detachTab(int index) {
    TabState& tab = tabs_.stateAt(index);
    QString draftId = tab.module + QStringLiteral(".__new_endpoint__");
    for (int suffix = 2;
         operationIds().contains(draftId) || tabs_.indexOf(TabState::Kind::Operation, draftId) >= 0;
         ++suffix) {
        draftId = tab.module + QStringLiteral(".__new_endpoint__%1").arg(suffix);
    }
    const QString name = tab.opName;
    detachDeletedOperation(tab, draftId);
    tabs_.refreshRow(index);
    if (index == activeTabIndex_) {
        restoreActiveTab();
    }
    persistOpenTabs();
    emit activeTabChanged();
    emit notify(tr("“%1” no longer exists; its unsaved edits stay open as a new endpoint. Save it "
                   "to create it again, or close the tab to discard them.")
                    .arg(name),
                false);
}

}  // namespace reqloom::desktop::qml
