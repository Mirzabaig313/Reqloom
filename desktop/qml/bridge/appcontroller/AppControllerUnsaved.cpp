// AppController — unsaved edits per tab: dirty tracking against each tab's edit baseline, the
// Save / Discard / Cancel prompt before edits are dropped, and keeping tabs pointed at operations
// that are renamed or deleted.
#include "AppController.h"

#include <QtCore/QMetaObject>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QStringList>

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
    // Deferred: mid-switch, the live fields briefly belong to neither tab.
    connect(this, &AppController::editChanged, this, [this]() {
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
    });

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

void AppController::updateActiveTabDirty() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    const bool dirty = tab.kind == TabState::Kind::Operation ? hasUnsavedEdits(liveOperationEdits())
                                                             : hasUnsavedEdits(tab);
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
        emit notify(tr("Save the new actor from its panel, then close it."), true);
        return false;
    }
    saveOperation();
    captureActiveTab();
    return !tabs_.stateAt(activeTabIndex_).dirty;
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
