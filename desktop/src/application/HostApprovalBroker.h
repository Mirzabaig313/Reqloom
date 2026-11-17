// HostApprovalBroker — the desktop app's answer to "may this project send credentials to this
// host?" (PRD FR-15.9). The engine asks on its worker thread; the broker hands the question to the
// GUI thread as a signal and blocks the worker until the person answers, the run is cancelled, or
// the broker is destroyed (both count as no). Asked on the GUI thread itself it never blocks: it
// answers Unavailable, since waiting there would freeze the app.
#pragma once

#include <reqloom/engine/HostApproval.h>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <condition_variable>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace reqloom::desktop {

/// The host key (`hostKeyForUrl`) of a URL the desktop app sends with Qt's network stack, only
/// when Qt's URL parser reads the same host and port as libcurl's; otherwise an error, so a URL
/// the two parsers disagree on is never approved as one host and sent to another.
[[nodiscard]] std::expected<std::string, QString> qtRequestHostKey(const QString& url);

class HostApprovalBroker final : public QObject, public engine::HostApprover {
    Q_OBJECT

public:
    explicit HostApprovalBroker(QObject* parent = nullptr);
    ~HostApprovalBroker() override;

    HostApprovalBroker(const HostApprovalBroker&) = delete;
    HostApprovalBroker& operator=(const HostApprovalBroker&) = delete;
    HostApprovalBroker(HostApprovalBroker&&) = delete;
    HostApprovalBroker& operator=(HostApprovalBroker&&) = delete;

    engine::HostApprovalAnswer ask(const engine::HostApprovalRequest& request) override;
    [[nodiscard]] std::string howToApprove(
        const engine::HostApprovalRequest& request) const override;

    /// Answer question `id`; unknown or already answered ids are ignored. GUI thread.
    void answer(int id, bool approve);

    /// Answer every open question with no, and every later one until `reopen` (a cancelled run,
    /// closing the app). Any thread.
    void declineAll();

    /// Ask the person again from now on; called when a run starts.
    void reopen();

signals:
    /// A question for the person; answer it with `answer(id, …)`. Emitted on the asking thread,
    /// delivered queued to receivers on the GUI thread.
    void approvalRequested(int id,
                           const QString& projectName,
                           const QString& host,
                           const QString& credential,
                           const QString& url);

private:
    std::mutex mutex_;
    std::condition_variable answered_;
    std::map<int, std::optional<bool>> open_;
    int nextId_{1};
    bool closing_{false};
};

}  // namespace reqloom::desktop
