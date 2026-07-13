// OAuth2AuthCodeFlow — see header. Interactive Authorization Code + PKCE flow.

#include "OAuth2AuthCodeFlow.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <array>

namespace reqloom::desktop {

namespace {
constexpr int kTimeoutMs = 180'000;  // 3 minutes for the user to authorize.

// base64url without padding (RFC 7636 §A).
[[nodiscard]] QString base64Url(const QByteArray& bytes) {
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
}  // namespace

OAuth2AuthCodeFlow::OAuth2AuthCodeFlow(QObject* parent) : QObject(parent) {}

OAuth2AuthCodeFlow::~OAuth2AuthCodeFlow() = default;

QString OAuth2AuthCodeFlow::randomVerifier() {
    // 64 random bytes → base64url gives a 43-128 char verifier (RFC 7636 §4.1).
    std::array<quint32, 16> words{};
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
    const QByteArray raw(reinterpret_cast<const char*>(words.data()),
                         static_cast<qsizetype>(words.size() * sizeof(quint32)));
    return base64Url(raw);
}

QString OAuth2AuthCodeFlow::challengeFor(const QString& verifier) {
    const QByteArray digest =
        QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
    return base64Url(digest);
}

void OAuth2AuthCodeFlow::start(const Config& config) {
    config_ = config;
    if (config_.authUrl.isEmpty() || config_.tokenUrl.isEmpty() || config_.clientId.isEmpty()) {
        finishErr(tr("Auth URL, Access Token URL and Client ID are required."));
        return;
    }

    const auto isLoopback = [](const QString& host) {
        return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") ||
               host == QLatin1String("::1");
    };
    // Secrets (code_verifier, client_secret, the token) cross authUrl/tokenUrl.
    // Require HTTPS; only loopback may use plain http (local test servers).
    const auto isSecure = [&](const QUrl& u) {
        return u.scheme() == QLatin1String("https") ||
               (u.scheme() == QLatin1String("http") && isLoopback(u.host()));
    };
    if (!isSecure(QUrl(config_.authUrl)) || !isSecure(QUrl(config_.tokenUrl))) {
        finishErr(tr("Auth URL and Access Token URL must use HTTPS (only loopback may use http)."));
        return;
    }

    const QUrl callback(config_.callbackUrl);
    if (!callback.isValid() || callback.port() <= 0 || callback.scheme() != QLatin1String("http") ||
        !isLoopback(callback.host())) {
        finishErr(
            tr("Callback URL must be a loopback address with a port, e.g. "
               "http://127.0.0.1:8080/callback."));
        return;
    }

    verifier_ = randomVerifier();
    state_ = base64Url(QUuid::createUuid().toRfc4122());

    server_ = std::make_unique<QTcpServer>();
    if (!server_->listen(QHostAddress::LocalHost, static_cast<quint16>(callback.port()))) {
        finishErr(
            tr("Could not bind the callback port %1 (already in use?).").arg(callback.port()));
        return;
    }
    connect(
        server_.get(), &QTcpServer::newConnection, this, &OAuth2AuthCodeFlow::onCallbackConnection);

    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this] {
        finishErr(tr("Timed out waiting for authorization."));
    });
    timeout_->start(kTimeoutMs);

    // Build the authorization request (RFC 6749 §4.1.1 + RFC 7636 §4.3).
    QUrl authUrl(config_.authUrl);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), config_.clientId);
    query.addQueryItem(QStringLiteral("redirect_uri"), config_.callbackUrl);
    query.addQueryItem(QStringLiteral("state"), state_);
    // PKCE (RFC 7636): S256 hashes the verifier; "plain" sends it as-is.
    const bool plain = config_.pkceMethod.compare(QLatin1String("plain"), Qt::CaseInsensitive) == 0;
    query.addQueryItem(QStringLiteral("code_challenge"),
                       plain ? verifier_ : challengeFor(verifier_));
    query.addQueryItem(QStringLiteral("code_challenge_method"),
                       plain ? QStringLiteral("plain") : QStringLiteral("S256"));
    if (!config_.scope.isEmpty()) {
        query.addQueryItem(QStringLiteral("scope"), config_.scope);
    }
    authUrl.setQuery(query);

    if (!QDesktopServices::openUrl(authUrl)) {
        finishErr(tr("Could not open the system browser."));
    }
}

void OAuth2AuthCodeFlow::onCallbackConnection() {
    QTcpSocket* socket = server_->nextPendingConnection();
    if (socket == nullptr) {
        return;
    }
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    // Buffer until the full request line arrives (a GET can span several reads);
    // handle at most once per connection.
    auto buffer = std::make_shared<QByteArray>();
    auto handled = std::make_shared<bool>(false);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, handled] {
        if (*handled) {
            return;
        }
        buffer->append(socket->readAll());
        const qsizetype eol = buffer->indexOf("\r\n");
        if (eol < 0) {
            return;  // request line not complete yet
        }
        *handled = true;

        // First line: "GET /callback?code=...&state=... HTTP/1.1".
        const QByteArray firstLine = buffer->left(eol);
        const auto parts = firstLine.split(' ');

        // Only the configured callback PATH is the genuine redirect. A browser
        // can open other connections to this loopback origin around the same
        // time (favicon, a reload, an OPTIONS preflight); if such a stray
        // request finished parsing first it would win the flow and fire a
        // spurious "no code" failure while the real callback is still in
        // flight. Reply 404 to anything that isn't the callback path (and to a
        // malformed request line) and keep listening — do NOT touch done_ or
        // server_ here.
        QString requestPath;
        if (parts.size() >= 2) {
            requestPath =
                QUrl(QStringLiteral("http://localhost") + QString::fromLatin1(parts.at(1))).path();
        }
        QString expectedPath = QUrl(config_.callbackUrl).path();
        if (expectedPath.isEmpty()) {
            expectedPath = QStringLiteral("/");
        }
        if (requestPath.isEmpty()) {
            requestPath = QStringLiteral("/");
        }
        if (parts.size() < 2 || requestPath != expectedPath) {
            const QByteArray notFound = QByteArrayLiteral("Not found.");
            socket->write(
                "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n"
                "Content-Length: " +
                QByteArray::number(notFound.size()) + "\r\n\r\n" + notFound);
            socket->flush();
            socket->disconnectFromHost();
            return;  // ignore; keep the server waiting for the real callback
        }

        // Genuine callback → parse the OAuth response parameters.
        const QUrl target(QStringLiteral("http://localhost") + QString::fromLatin1(parts.at(1)));
        const QUrlQuery q(target);
        const QString code = q.queryItemValue(QStringLiteral("code"));
        const QString returnedState = q.queryItemValue(QStringLiteral("state"));
        const QString providerError = q.queryItemValue(QStringLiteral("error"));

        const bool ok = providerError.isEmpty() && !code.isEmpty() && returnedState == state_;
        const QByteArray page =
            ok ? QByteArrayLiteral("Authorization complete. You can close this tab.")
               : QByteArrayLiteral("Authorization failed. You can close this tab.");
        socket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
            "Content-Length: " +
            QByteArray::number(page.size()) + "\r\n\r\n" + page);
        socket->flush();
        socket->disconnectFromHost();

        // Stop accepting further loopback connections before the async token
        // exchange, so a stray request (favicon, reload) can't fire a spurious
        // failure mid-flight.
        if (server_) {
            server_->close();
        }

        if (!providerError.isEmpty()) {
            finishErr(tr("Authorization denied: %1").arg(providerError));
        } else if (returnedState != state_) {
            finishErr(tr("State mismatch — possible CSRF; aborting."));
        } else if (code.isEmpty()) {
            finishErr(tr("No authorization code returned."));
        } else {
            exchangeCode(code);
        }
    });
}

void OAuth2AuthCodeFlow::exchangeCode(const QString& code) {
    if (net_ == nullptr) {
        net_ = new QNetworkAccessManager(this);
    }
    // RFC 6749 §4.1.3 + RFC 7636 §4.5 — code_verifier proves possession.
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    form.addQueryItem(QStringLiteral("code"), code);
    form.addQueryItem(QStringLiteral("redirect_uri"), config_.callbackUrl);
    form.addQueryItem(QStringLiteral("client_id"), config_.clientId);
    form.addQueryItem(QStringLiteral("code_verifier"), verifier_);

    QNetworkRequest request{QUrl(config_.tokenUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Accept", "application/json");

    // Client authentication (RFC 6749 §2.3.1):
    //   basic → HTTP Basic header, creds omitted from the body;
    //   none  → public client, no secret at all (PKCE);
    //   body  → client_secret in the body (default).
    if (config_.clientAuth == QLatin1String("basic") && !config_.clientSecret.isEmpty()) {
        const QByteArray creds =
            (config_.clientId + QLatin1Char(':') + config_.clientSecret).toUtf8();
        request.setRawHeader("Authorization", "Basic " + creds.toBase64());
    } else if (config_.clientAuth != QLatin1String("none") && !config_.clientSecret.isEmpty()) {
        form.addQueryItem(QStringLiteral("client_secret"), config_.clientSecret);
    }

    QNetworkReply* reply = net_->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            finishErr(tr("Token request failed: %1").arg(reply->errorString()));
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            finishErr(tr("Token endpoint did not return a JSON object."));
            return;
        }
        const auto token = doc.object().value(QStringLiteral("access_token")).toString();
        if (token.isEmpty()) {
            finishErr(tr("Token endpoint response had no access_token."));
            return;
        }
        finishOk(token);
    });
}

void OAuth2AuthCodeFlow::finishOk(const QString& token) {
    if (done_) {
        return;
    }
    done_ = true;
    if (server_) {
        server_->close();
    }
    emit succeeded(token);
}

void OAuth2AuthCodeFlow::finishErr(const QString& error) {
    if (done_) {
        return;
    }
    done_ = true;
    if (server_) {
        server_->close();
    }
    emit failed(error);
}

}  // namespace reqloom::desktop
