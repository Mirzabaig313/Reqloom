// HostApprovalBroker — see header.
#include "HostApprovalBroker.h"

#include <QtCore/QThread>
#include <QtCore/QUrl>

#include <utility>

namespace reqloom::desktop {

namespace ce = reqloom::engine;

std::expected<std::string, QString> qtRequestHostKey(const QString& url) {
    auto key = ce::hostKeyForUrl(url.toStdString());
    if (!key) {
        return std::unexpected(QString::fromStdString(key.error().detail));
    }
    const QUrl parsed{url, QUrl::StrictMode};
    const QString scheme = parsed.scheme().toLower();
    if (!parsed.isValid() ||
        (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        return std::unexpected(QStringLiteral("the URL '%1' is not a valid http(s) URL").arg(url));
    }
    QString host = parsed.host(QUrl::FullyEncoded).toLower();
    if (host.contains(QLatin1Char(':'))) {
        host = QLatin1Char('[') + host + QLatin1Char(']');  // IPv6 literal
    }
    while (host.endsWith(QLatin1Char('.'))) {
        host.chop(1);
    }
    const int port = parsed.port(scheme == QStringLiteral("https") ? 443 : 80);
    const QString qtKey =
        scheme + QStringLiteral("://") + host + QLatin1Char(':') + QString::number(port);
    if (qtKey.toStdString() != *key) {
        return std::unexpected(
            QStringLiteral("the URL '%1' is read as host %2 by one parser and %3 by another, so "
                           "it was not used")
                .arg(url, qtKey, QString::fromStdString(*key)));
    }
    return std::move(*key);
}

HostApprovalBroker::HostApprovalBroker(QObject* parent) : QObject(parent) {}

HostApprovalBroker::~HostApprovalBroker() {
    declineAll();
}

ce::HostApprovalAnswer HostApprovalBroker::ask(const ce::HostApprovalRequest& request) {
    if (QThread::currentThread() == thread()) {
        return ce::HostApprovalAnswer::Unavailable;
    }
    int id{};
    {
        const std::scoped_lock lock{mutex_};
        if (closing_) {
            return ce::HostApprovalAnswer::Deny;
        }
        id = nextId_++;
        open_.emplace(id, std::nullopt);
    }
    emit approvalRequested(id,
                           QString::fromStdString(request.projectName),
                           QString::fromStdString(request.host),
                           QString::fromStdString(request.credential),
                           QString::fromStdString(request.url));
    std::unique_lock lock{mutex_};
    answered_.wait(lock, [&] { return closing_ || open_.at(id).has_value(); });
    const bool approved = !closing_ && open_.at(id).value_or(false);
    open_.erase(id);
    return approved ? ce::HostApprovalAnswer::Approve : ce::HostApprovalAnswer::Deny;
}

std::string HostApprovalBroker::howToApprove(const ce::HostApprovalRequest& request) const {
    return "Run it again and choose Approve when the app asks about " + request.host + ".";
}

void HostApprovalBroker::answer(const int id, const bool approve) {
    {
        const std::scoped_lock lock{mutex_};
        const auto it = open_.find(id);
        if (it == open_.end() || it->second.has_value()) {
            return;
        }
        it->second = approve;
    }
    answered_.notify_all();
}

void HostApprovalBroker::declineAll() {
    {
        const std::scoped_lock lock{mutex_};
        closing_ = true;
        for (auto& entry : open_) {
            if (!entry.second) {
                entry.second = false;
            }
        }
    }
    answered_.notify_all();
}

void HostApprovalBroker::reopen() {
    const std::scoped_lock lock{mutex_};
    closing_ = false;
}

}  // namespace reqloom::desktop
