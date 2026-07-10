// OAuth2AuthCodeFlow — runs the OAuth 2.0 Authorization Code flow with PKCE
// (RFC 7636) in-app: generates the PKCE pair, opens the system browser to the
// authorization endpoint, catches the redirect on a loopback server, then
// exchanges the code for an access token. Emits `succeeded` or `failed` exactly
// once. Desktop-only — the headless engine can't drive an interactive browser.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

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

signals:
    void succeeded(const QString& accessToken);
    void failed(const QString& error);

private:
    void onCallbackConnection();
    void exchangeCode(const QString& code);
    void finishOk(const QString& token);
    void finishErr(const QString& error);

    [[nodiscard]] static QString randomVerifier();
    [[nodiscard]] static QString challengeFor(const QString& verifier);

    Config config_;
    QString verifier_;
    QString state_;
    std::unique_ptr<QTcpServer> server_;
    QNetworkAccessManager* net_{nullptr};
    QTimer* timeout_{nullptr};
    bool done_{false};
};

}  // namespace reqloom::desktop
