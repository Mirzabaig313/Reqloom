// TabEdits — pure rules for a tab's unsaved edits: when a tab is dirty, how a renamed or deleted
// operation reaches the open tabs, which saved tabs can reopen, and what the unsaved-changes prompt
// says.
#pragma once

#include "TabModel.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <functional>
#include <vector>

namespace reqloom::desktop::qml {

/// True when two operation snapshots hold the same savable edits. The response, run timeline,
/// ephemeral OAuth2 access token and the chain editor's candidate lists are ignored.
[[nodiscard]] bool sameOperationEdits(const TabState& a, const TabState& b);

/// Whether closing `tab` would lose edits. An operation tab is dirty while its edits differ from
/// the baseline taken when editing began; a never-saved endpoint draft is dirty once it is named
/// or any field differs from the blank draft; a never-saved actor draft is always dirty.
[[nodiscard]] bool hasUnsavedEdits(const TabState& tab);

/// Re-point `tab` from operation `oldId` to `newId`: the tab's own identity when it is that
/// operation's tab, a draft's module when the module was renamed, and every dependency on the
/// operation in the live edits and the baseline alike, so a rename alone never makes a tab dirty.
/// Returns true when anything changed.
bool renameOperationInTab(TabState& tab, const QString& oldId, const QString& newId);

/// Drop every dependency on the deleted operation `operationId` from `tab`, live and baseline, as
/// the project drops it from the saved depends_on. Returns true when anything changed.
bool forgetOperationInTab(TabState& tab, const QString& operationId);

/// Turn the tab of a deleted operation into a never-saved endpoint draft identified by `draftId`,
/// keeping its edits and name so the person can save it again or discard it.
void detachDeletedOperation(TabState& tab, const QString& draftId);

/// A saved tab descriptor as persisted per project.
struct SavedTab {
    int kind{0};
    QString id;
};

/// Saved tabs that can reopen, and how many cannot.
struct TabRestorePlan {
    std::vector<SavedTab> reopen;
    int dropped{0};
};

/// Split `saved` by `canReopen` (which also rejects malformed ids).
[[nodiscard]] TabRestorePlan planTabRestore(const std::vector<SavedTab>& saved,
                                            const std::function<bool(const SavedTab&)>& canReopen);

/// The toast for `dropped` saved tabs that could not reopen.
[[nodiscard]] QString droppedTabsMessage(int dropped);

/// What is about to drop the tabs' edits; it words the prompt.
enum class UnsavedReason : std::uint8_t { CloseTabs, LeaveProject, Quit };

/// The texts of the unsaved-changes dialog.
struct UnsavedPrompt {
    QString title;
    QString message;
    QString saveText;
    QString discardText;
    QString cancelText;
};

/// The dialog texts for the unsaved tabs titled `titles` (one or more). `cancelsRun` adds that the
/// run in progress in a closing tab is cancelled.
[[nodiscard]] UnsavedPrompt unsavedPrompt(const QStringList& titles,
                                          UnsavedReason reason,
                                          bool cancelsRun);

/// Save each tab in `uids` in order with `save`, stopping at the first failure. Returns true when
/// every save succeeded.
bool saveInOrder(const std::vector<std::uint64_t>& uids,
                 const std::function<bool(std::uint64_t)>& save);

/// The run statuses the chain graph shows: the live map while the tab on screen owns the latest
/// run (or neither a tab nor the run has one), else the statuses that tab kept from its own run.
[[nodiscard]] const QHash<QString, QString>& shownChainStatus(const TabState* active,
                                                              bool activeOwnsRun,
                                                              bool runHasTab,
                                                              const QHash<QString, QString>& live);

}  // namespace reqloom::desktop::qml
