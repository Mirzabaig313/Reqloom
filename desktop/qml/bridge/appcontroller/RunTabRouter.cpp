// RunTabRouter — see header.
#include "RunTabRouter.h"

#include <utility>

namespace reqloom::desktop::qml {

void RunTabRouter::begin(TabKey origin) {
    origin_ = std::move(origin);
    inFlight_ = true;
    discarded_ = false;
    hiddenLoaded_ = false;
    hidden_.reset();
    target_.clear();
    stepOps_.clear();
}

void RunTabRouter::end() {
    inFlight_ = false;
    hiddenLoaded_ = false;
    hidden_.reset();
    target_.clear();
    stepOps_.clear();
}

void RunTabRouter::discard() {
    discarded_ = true;
    hiddenLoaded_ = false;
    hidden_.reset();
}

bool RunTabRouter::originIs(const TabKey& key) const {
    return inFlight_ && !discarded_ && origin_.valid() && origin_ == key;
}

TimelineModel* RunTabRouter::timeline(TimelineModel& visible,
                                      bool shown,
                                      const TimelineModel::Snapshot* stored) {
    if (!inFlight_ || discarded_) {
        return nullptr;
    }
    if (!origin_.valid()) {
        return &visible;
    }
    if (shown) {
        if (auto gathered = takeHiddenTimeline()) {
            visible.restoreSnapshot(std::move(*gathered));
        }
        return &visible;
    }
    if (stored == nullptr) {
        return nullptr;
    }
    if (!hiddenLoaded_) {
        hidden_.restoreSnapshot(*stored);
        hiddenLoaded_ = true;
    }
    return &hidden_;
}

std::optional<TimelineModel::Snapshot> RunTabRouter::takeHiddenTimeline() {
    if (!hiddenLoaded_) {
        return std::nullopt;
    }
    TimelineModel::Snapshot gathered = hidden_.takeSnapshot();
    hiddenLoaded_ = false;
    hidden_.reset();
    return gathered;
}

void RunTabRouter::setTarget(QString operationId) {
    target_ = std::move(operationId);
    stepOps_.clear();
}

void RunTabRouter::noteStep(int index, const QString& operationId) {
    stepOps_.insert(index, operationId);
}

QString RunTabRouter::stepOperation(int index) const {
    return stepOps_.value(index);
}

bool RunTabRouter::isTargetStep(int index) const {
    return inFlight_ && !target_.isEmpty() && stepOps_.value(index) == target_;
}

}  // namespace reqloom::desktop::qml
