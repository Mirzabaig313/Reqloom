// OAuth2AuthCodeFlow — in-app OAuth 2.0 Authorization Code flow with PKCE (RFC 7636): opens the
// browser, catches the redirect on a loopback server, exchanges the code. Emits `succeeded` or
// `failed` exactly once.
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

#include <expected>
#include <memory>
#include <vector>

class QTcpServer;
class QNetworkAccessManager;
class QTimer;

namespace reqloom::desktop {

class OAuth2AuthCodeFlow : public QObject {
    Q_OBJECT

public:
    struct Config {
        QString authUrl;
        QString tokenUrl;
        QString clientId;
        QString clientSecret;  ///< Optional; public clients omit it (PKCE only).
        QString scope;         ///< Optional, space-separated.
        QString callbackUrl;   ///< Loopback redirect, e.g. http://127.0.0.1:8080/callback.
        QString clientAuth;    ///< Token exchange: "basic" | "body" (default) | "none".
        QString pkceMethod;    ///< "S256" (default) | "plain".
    };

    explicit OAuth2AuthCodeFlow(QObject* parent = nullptr);
    ~OAuth2AuthCodeFlow() override;

    OAuth2AuthCodeFlow(const OAuth2AuthCodeFlow&) = delete;
    OAuth2AuthCodeFlow& operator=(const OAuth2AuthCodeFlow&) = delete;

    /// Begin the flow. Validates config, binds the loopback server, and opens
    /// the browser. Errors (bad config, port in use) surface via `failed`.
    void start(const Config& config);

    /// Loopback addresses a redirect host can resolve to: both families for `localhost`,
    /// the literal itself for `127.0.0.1` or `::1`, and nothing for any other host.
    [[nodiscard]] static QList<QHostAddress> loopbackAddressesFor(const QString& host);

    /// Listen on every loopback address of `host` at `port`. An address family the
    /// machine lacks is skipped; a port held by another process on any family fails.
    [[nodiscard]] static std::expected<std::vector<std::unique_ptr<QTcpServer>>, QString>
    listenLoopback(const QString& host, quint16 port);

signals:
    void succeeded(const QString& accessToken);
    void failed(const QString& error);

private:
    void onCallbackConnection(QTcpServer* server);
    void closeServers();
    void exchangeCode(const QString& code);
    void finishOk(const QString& token);
    void finishErr(const QString& error);

    [[nodiscard]] static QString randomVerifier();
    [[nodiscard]] static QString challengeFor(const QString& verifier);

    Config config_;
    QString verifier_;
    QString state_;
    std::vector<std::unique_ptr<QTcpServer>> servers_;
    QNetworkAccessManager* net_{nullptr};
    QTimer* timeout_{nullptr};
    bool done_{false};
};

}  // namespace reqloom::desktop
