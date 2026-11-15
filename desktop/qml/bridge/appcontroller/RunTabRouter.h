// RunTabRouter — keeps one run's events with the editor tab that started it, so switching tabs
// mid-run neither moves the run into another tab nor leaves its own tab stuck on "running".
#pragma once

#include "TimelineModel.h"

#include <QtCore/QHash>
#include <QtCore/QString>

#include <optional>

namespace reqloom::desktop::qml {

class RunTabRouter {
public:
    /// Identity of an editor tab that survives reordering and reopening: its kind and id.
    struct TabKey {
        int kind{-1};
        QString id;
        [[nodiscard]] bool valid() const { return kind >= 0; }
        bool operator==(const TabKey&) const = default;
    };

    /// A run was accepted while `origin` was the shown tab; an invalid key means no tab was open.
    void begin(TabKey origin);
    /// The run's final report has been handled; nothing routes until the next `begin`.
    void end();
    /// The origin tab closed: the run's remaining events are dropped.
    void discard();

    [[nodiscard]] bool inFlight() const { return inFlight_; }
    [[nodiscard]] bool discarded() const { return discarded_; }
    [[nodiscard]] const TabKey& origin() const { return origin_; }
    /// True while a run is in flight from the tab `key`.
    [[nodiscard]] bool originIs(const TabKey& key) const;
    /// The origin tab's operation was renamed: the run in flight stays with that tab. Steps keep
    /// the id the engine started them under.
    void renameOrigin(const TabKey& from, const TabKey& to);

    /// The timeline the run's next event belongs in, or nullptr when it belongs nowhere.
    ///
    /// @param visible  The timeline on screen.
    /// @param shown    Whether the origin tab is the shown tab.
    /// @param stored   The origin tab's parked timeline; nullptr when that tab is gone.
    [[nodiscard]] TimelineModel* timeline(TimelineModel& visible,
                                          bool shown,
                                          const TimelineModel::Snapshot* stored);
    /// The timeline gathered while the origin tab was hidden, for the tab to park or show; nullopt
    /// when nothing was gathered.
    [[nodiscard]] std::optional<TimelineModel::Snapshot> takeHiddenTimeline();

    void setTarget(QString operationId);
    void noteStep(int index, const QString& operationId);
    [[nodiscard]] QString stepOperation(int index) const;
    /// True when step `index` runs the run's target, the only step whose response the panel shows.
    [[nodiscard]] bool isTargetStep(int index) const;
    /// True when a response at step `index` belongs in the response panel: the target's own
    /// response or poll attempt, never a sign-in, refresh or token response stamped with its step.
    /// `exchange` is the engine's `exchangeKindName`.
    [[nodiscard]] bool showsResponse(int index, const QString& exchange) const;
    /// True for the operation's own request or a poll attempt (`exchangeKindName`).
    [[nodiscard]] static bool isOwnExchange(const QString& exchange);

private:
    TabKey origin_;
    bool inFlight_{false};
    bool discarded_{false};
    bool hiddenLoaded_{false};
    TimelineModel hidden_;
    QString target_;
    QHash<int, QString> stepOps_;
};

}  // namespace reqloom::desktop::qml
