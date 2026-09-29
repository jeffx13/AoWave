#include "trackers/tracker.h"
#include "core/logger.h"
#include "core/appshell.h"
#include "core/settings.h"
#include "platform/platform.h"
#include <QDateTime>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <memory>
#include <QTimer>
#include <QUrlQuery>

QString Tracker::settingsGroup() const { return QStringLiteral("trackers/") + name(); }

// A bearer token is the account until it expires, so it goes through DPAPI.
void Tracker::storeToken(const Grant &grant) {
    Settings &s = Settings::instance();
    const QString sealed = Platform::protect(grant.token);
    const QString sealedRefresh = Platform::protect(grant.refresh);
    if (!grant.token.isEmpty() && sealed.isEmpty()) {
        // No plain-text fallback: a token the user cannot tell is exposed is worse.
        logWarn() << name() << "could not protect the token; not storing it. "
                               "Sign-in will not survive a restart.";
    } else {
        s.setValue(settingsGroup() + QStringLiteral("/tokenEnc"), sealed);
        s.setValue(settingsGroup() + QStringLiteral("/refreshEnc"), sealedRefresh);
        s.setValue(settingsGroup() + QStringLiteral("/expires"), grant.expiresAt);
    }
    {
        QMutexLocker lock(&m_authMutex);
        m_token = grant.token;
        m_refreshToken = grant.refresh;
        m_expiresAt = grant.expiresAt;
    }
    emit authChanged();
}

void Tracker::loadToken() {
    Settings &s = Settings::instance();
    m_accountName = s.value(settingsGroup() + QStringLiteral("/account")).toString();
    QMutexLocker lock(&m_authMutex);
    m_clientId = s.value(settingsGroup() + QStringLiteral("/clientId")).toString().trimmed();
    // The secret is a credential too.
    m_clientSecret = Platform::unprotect(
        s.value(settingsGroup() + QStringLiteral("/clientSecretEnc")).toString());
    m_expiresAt = s.value(settingsGroup() + QStringLiteral("/expires")).toLongLong();
    m_token = Platform::unprotect(s.value(settingsGroup() + QStringLiteral("/tokenEnc")).toString());
    m_refreshToken = Platform::unprotect(s.value(settingsGroup() + QStringLiteral("/refreshEnc")).toString());
}

QString Tracker::accessToken() const {
    QMutexLocker lock(&m_authMutex);
    return m_token;
}

QString Tracker::clientId() const {
    QMutexLocker lock(&m_authMutex);
    return m_clientId;
}

QString Tracker::clientSecret() const {
    QMutexLocker lock(&m_authMutex);
    return m_clientSecret;
}

QString Tracker::freshToken(Client *client) {
    QMutexLocker refreshing(&m_refreshMutex);
    QString refresh;
    {
        QMutexLocker lock(&m_authMutex);
        const bool expiring = m_expiresAt > 0 && QDateTime::currentSecsSinceEpoch() > m_expiresAt - 300;
        if (m_token.isEmpty() || !expiring || m_refreshToken.isEmpty()) return m_token;
        refresh = m_refreshToken;
    }
    const Grant grant = refreshGrant(client, refresh);
    if (grant.token.isEmpty()) {
        logWarn() << name() << "token refresh failed; sign in again";
        return {};
    }
    QMetaObject::invokeMethod(this, [this, grant]() { storeToken(grant); }, Qt::QueuedConnection);
    QMutexLocker lock(&m_authMutex);
    m_token = grant.token;
    m_refreshToken = grant.refresh;
    m_expiresAt = grant.expiresAt;
    return m_token;
}

void Tracker::shutdown() {
    m_cancel.cancel();
    m_runs.waitAll("Tracker request");
}

void Tracker::setClientId(const QString &id) {
    const QString trimmed = id.trimmed();
    {
        QMutexLocker lock(&m_authMutex);
        if (trimmed == m_clientId) return;
        m_clientId = trimmed;
    }
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/clientId"), trimmed);
    // A different application is a different set of tokens.
    if (isAuthenticated()) signOut();
    else                   emit authChanged();
}

void Tracker::setClientSecret(const QString &secret) {
    const QString trimmed = secret.trimmed();
    {
        QMutexLocker lock(&m_authMutex);
        if (trimmed == m_clientSecret) return;
        m_clientSecret = trimmed;
    }
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/clientSecretEnc"),
                                  Platform::protect(trimmed));
    if (isAuthenticated()) signOut();
    else                   emit authChanged();
}

void Tracker::setAccountName(const QString &accountName) {
    if (accountName == m_accountName) return;
    m_accountName = accountName;
    Settings::instance().setValue(settingsGroup() + QStringLiteral("/account"), accountName);
    emit authChanged();
}

void Tracker::signOut() {
    Settings &s = Settings::instance();
    for (const char *key : {"/tokenEnc", "/refreshEnc", "/expires", "/account"})
        s.setValue(settingsGroup() + QLatin1String(key), QString());
    {
        QMutexLocker lock(&m_authMutex);
        m_token.clear();
        m_refreshToken.clear();
        m_expiresAt = 0;
    }
    m_accountName.clear();
    emit authChanged();
}

LoopbackAuth *LoopbackAuth::start(QObject *parent, QPointer<LoopbackAuth> &pending, int port,
                                  const QString &service) {
    if (pending) {
        delete pending.data();   // frees the port before the new listener binds it
        pending = nullptr;
    }
    auto *loopback = new LoopbackAuth(parent);
    pending = loopback;
    connect(loopback, &LoopbackAuth::failed, loopback, [loopback, service](const QString &why) {
        logWarn() << service << "sign-in failed:" << why;
        AppShell::instance().reportError(
            tr("%1 sign-in did not complete (%2).").arg(service, why), service);
        loopback->deleteLater();
    });
    if (loopback->listen(port) == 0) {
        loopback->deleteLater();
        return nullptr;
    }
    return loopback;
}

LoopbackAuth::LoopbackAuth(QObject *parent) : QObject(parent) {
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    m_state = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(words), sizeof words).toHex());
}

void LoopbackAuth::serve(QTcpSocket *socket, const QByteArray &contentType,
                         const QByteArray &body) {
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + contentType
                  + "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
}

namespace {

// An implicit grant hands the token back in the fragment, which a browser never sends to a
// server. This page is what the redirect lands on: it reposts the fragment as a query on the
// same port, which is the only way a desktop app can read it.
QByteArray bridgePage() {
    return QByteArrayLiteral(
        "<!doctype html><meta charset=\"utf-8\"><title>" APP_NAME "</title>"
        "<body style=\"font:16px system-ui;padding:3rem;text-align:center\">"
        "<p id=\"m\">Finishing sign-in...</p><script>"
        "var h=location.hash.slice(1);"
        "if(h){location.replace('/?'+h);}"
        "else{document.getElementById('m').textContent="
        "'That redirect carried no token. You can close this tab.';}"
        "</script></body>");
}

QByteArray donePage(const QString &message) {
    return QByteArrayLiteral("<!doctype html><meta charset=\"utf-8\"><title>" APP_NAME "</title>"
                             "<body style=\"font:16px system-ui;padding:3rem;text-align:center\"><p>")
           + message.toHtmlEscaped().toUtf8() + QByteArrayLiteral("</p></body>");
}

// "GET /?code=... HTTP/1.1" - the request line is all this needs.
QUrl targetOf(const QByteArray &request) {
    const QString text = QString::fromUtf8(request);
    const int start = text.indexOf(QLatin1String("GET "));
    const int end = text.indexOf(QLatin1String(" HTTP"), start);
    if (start < 0 || end <= start) return {};
    return QUrl(QStringLiteral("http://127.0.0.1") + text.mid(start + 4, end - start - 4));
}

}

// Stays open until a redirect carries something usable: an implicit grant needs two requests,
// and a user who closed the first tab gets to try again on the same listener.
int LoopbackAuth::listen(int port, int timeoutMs) {
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, quint16(port))) {
        emit failed(port == 0
                        ? tr("could not open a loopback port")
                        : tr("port %1 is already in use - another sign-in, or another "
                             "program, is holding it").arg(port));
        server->deleteLater();
        return 0;
    }
    port = server->serverPort();

    connect(server, &QTcpServer::newConnection, this, [this, server]() {
        QTcpSocket *socket = server->nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, server, buffer]() {
            buffer->append(socket->readAll());
            // A request line can arrive split across packets.
            if (!buffer->contains('\n')) return;

            const QUrl target = targetOf(*buffer);
            // A browser asks for /favicon.ico off its own bat, and treating that as an empty
            // redirect would end the sign-in a moment after starting it.
            const QString path = target.path();
            if (!path.isEmpty() && path != QLatin1String("/")) {
                socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }

            const QUrlQuery query(target);
            const QString code  = query.queryItemValue(QStringLiteral("code"));
            const QString token = query.queryItemValue(QStringLiteral("access_token"));
            QString error = query.queryItemValue(QStringLiteral("error_description"),
                                                 QUrl::FullyDecoded);
            if (error.isEmpty()) error = query.queryItemValue(QStringLiteral("error"));

            if ((!code.isEmpty() || !token.isEmpty() || !error.isEmpty())
                && query.queryItemValue(QStringLiteral("state")) != m_state) {
                socket->write("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (!code.isEmpty() || !token.isEmpty()) {
                serve(socket, "text/html; charset=utf-8",
                      donePage(QStringLiteral("Signed in. You can close this tab and return to "
                                              APP_NAME ".")));
                server->close();
                if (!code.isEmpty()) emit codeReceived(code);
                else emit tokenReceived(token,
                                        query.queryItemValue(QStringLiteral("expires_in")).toLongLong());
                return;
            }
            if (!error.isEmpty()) {
                serve(socket, "text/html; charset=utf-8",
                      donePage(QStringLiteral("Sign-in was refused. You can close this tab.")));
                server->close();
                emit failed(error);
                return;
            }
            // Nothing in the query: either the token is still in the fragment, or it never came.
            if (!m_servedBridge) {
                m_servedBridge = true;
                serve(socket, "text/html; charset=utf-8", bridgePage());
                return;
            }
            serve(socket, "text/html; charset=utf-8",
                  donePage(QStringLiteral("That redirect carried no sign-in. You can close this "
                                          "tab.")));
            server->close();
            emit failed(tr("the redirect carried no code or token"));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });

    // Never leave a port open for a sign-in the user abandoned.
    QTimer::singleShot(timeoutMs, server, [this, server, timeoutMs]() {
        if (!server->isListening()) return;
        server->close();
        emit failed(tr("nothing came back within %1 minutes - the browser never returned to %2")
                        .arg(qMax(1, timeoutMs / 60000)).arg(QStringLiteral(APP_NAME)));
    });
    return port;
}
