#include "trackers/trakt.h"
#include <QGuiApplication>
#include <QClipboard>
#include "core/appshell.h"
#include "core/logger.h"
#include "core/settings.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>
#include <QUrl>

namespace {
constexpr const char *kApi = "https://api.trakt.tv";
}

Trakt::Trakt(QObject *parent) : Tracker(parent) { loadToken(); }

void Trakt::authenticate() {
    if (!canAuthenticate()) {
        AppShell::instance().reportError(registrationHint(), name());
        return;
    }
    runInBackground([this, id = clientId()](const CancelToken &cancel) {
        Client client(cancel);
        const auto response = client.post(QString::fromLatin1(kApi) + QStringLiteral("/oauth/device/code"),
                                          QStringLiteral(R"({"client_id":"%1"})").arg(id).toUtf8(),
                                          {{"Content-Type", "application/json"}});
        const QJsonObject json = response.toJsonObject();
        const QString deviceCode = json.value("device_code").toString();
        const QString userCode = json.value("user_code").toString();
        const QString url = json.value("verification_url").toString();
        const int interval = qMax(1, json.value("interval").toInt(5));
        const int expires = json.value("expires_in").toInt(600);
        if (deviceCode.isEmpty()) {
            if (cancel.isCancelled()) return;
            logWarn() << "Trakt" << "device code request failed:" << response.code << response.error;
            AppShell::instance().reportError(
                tr("Trakt would not start a sign-in (HTTP %1). Check the Client ID.")
                    .arg(response.code), QStringLiteral("Trakt"));
            return;
        }

        QMetaObject::invokeMethod(this, [this, deviceCode, userCode, url, interval, expires]() {
            emit deviceCodeReady(userCode, url);
            // The page asks for this code, so it has to reach the user.
            QGuiApplication::clipboard()->setText(userCode);
            AppShell::instance().reportInfo(
                tr("Enter the code %1 on the Trakt page that just opened (%2). "
                   "It is on your clipboard.").arg(userCode, url),
                QStringLiteral("Trakt"));
            QDesktopServices::openUrl(QUrl(url));
            pollForToken(deviceCode, interval, expires);
        }, Qt::QueuedConnection);
    });
}

void Trakt::pollForToken(const QString &deviceCode, int intervalSecs, int expiresIn) {
    m_poll.stop();
    m_poll.setInterval(intervalSecs * 1000);
    const qint64 deadline = QDateTime::currentSecsSinceEpoch() + expiresIn;

    disconnect(&m_poll, nullptr, this, nullptr);
    connect(&m_poll, &QTimer::timeout, this, [this, deviceCode, deadline]() {
        if (QDateTime::currentSecsSinceEpoch() > deadline) {
            m_poll.stop();
            logWarn() << "Trakt" << "sign-in expired";
            AppShell::instance().reportError(
                tr("The Trakt code expired before it was entered. Sign in again."),
                QStringLiteral("Trakt"));
            return;
        }
        runInBackground(
            [this, deviceCode, id = clientId(), secret = clientSecret()](const CancelToken &cancel) {
            Client client(cancel);
            const auto response = client.post(
                QString::fromLatin1(kApi) + QStringLiteral("/oauth/device/token"),
                QStringLiteral(R"({"code":"%1","client_id":"%2","client_secret":"%3"})")
                    .arg(deviceCode, id, secret).toUtf8(),
                {{"Content-Type", "application/json"}});
            // 400 is "not yet" and 429 is "too often"; 404, 409, 410 and 418 are terminal.
            if (response.code == 400 || response.code == 429) return;
            const QJsonObject json = response.toJsonObject();
            const QString token = json.value("access_token").toString();
            QMetaObject::invokeMethod(this, [this, token, json]() {
                m_poll.stop();
                if (token.isEmpty()) {
                    logWarn() << "Trakt" << "sign-in refused";
                    AppShell::instance().reportError(tr("Trakt refused the sign-in."),
                                                     QStringLiteral("Trakt"));
                    return;
                }
                storeToken({token, json.value("refresh_token").toString(),
                            QDateTime::currentSecsSinceEpoch() + json.value("expires_in").toInt()});
                fetchViewer();
            }, Qt::QueuedConnection);
        });
    });
    m_poll.start();
}

QList<Tracker::Result> Trakt::search(Client *client, const QString &query) {
    const auto response = client->withSession(QStringLiteral("trakt"))
        .get(QString::fromLatin1(kApi) + QStringLiteral("/search/show,movie"),
             {{"trakt-api-version", "2"}, {"trakt-api-key", clientId()}},
             {{"query", query}, {"limit", "20"}});

    QList<Result> results;
    for (const QJsonValue &value : response.toJsonArray()) {
        const QJsonObject hit = value.toObject();
        const QJsonObject node = hit.contains("show") ? hit.value("show").toObject()
                                                      : hit.value("movie").toObject();
        if (node.isEmpty()) continue;
        Result result;
        result.remoteId = QString::number(node.value("ids").toObject().value("trakt").toInt());
        result.title = node.value("title").toString();
        const int year = node.value("year").toInt();
        if (year > 0) result.year = QString::number(year);
        results.append(result);
    }
    return results;
}

// The settings endpoint is the only thing that names the account.
void Trakt::fetchViewer() {
    runInBackground([this, token = accessToken(), id = clientId()](const CancelToken &cancel) {
        Client client(cancel);
        const auto response = client.get(QString::fromLatin1(kApi) + QStringLiteral("/users/settings"),
                                         {{"Authorization", QStringLiteral("Bearer ") + token},
                                          {"trakt-api-version", "2"},
                                          {"trakt-api-key", id}});
        const QString name = response.toJsonObject().value("user").toObject()
                                 .value("username").toString();
        if (name.isEmpty()) return;
        QMetaObject::invokeMethod(this, [this, name]() { setAccountName(name); },
                                  Qt::QueuedConnection);
    });
}

// Trakt tokens last three months and it issues a refresh token.
Tracker::Grant Trakt::refreshGrant(Client *client, const QString &refreshToken) {
    const QByteArray body = QStringLiteral(
        R"({"refresh_token":"%1","client_id":"%2","client_secret":"%3",)"
        R"("grant_type":"refresh_token","redirect_uri":"urn:ietf:wg:oauth:2.0:oob"})")
        .arg(refreshToken, clientId(), clientSecret()).toUtf8();
    const auto response = client->post(QString::fromLatin1(kApi) + QStringLiteral("/oauth/token"),
                                       body, {{"Content-Type", "application/json"}});
    const QJsonObject json = response.toJsonObject();
    return {json.value("access_token").toString(), json.value("refresh_token").toString(),
            QDateTime::currentSecsSinceEpoch() + json.value("expires_in").toInt()};
}

// No single "list entry" object: watched/shows, watchlist and ratings are reconciled.
// Watched wins over watchlist.

Tracker::Entry Trakt::entryFor(Client *client, const QString &remoteId) {
    Entry entry;
    entry.remoteId = remoteId;
    const QString token = freshToken(client);
    if (token.isEmpty()) return entry;

    const QMap<QString, QString> auth{
        {"Authorization", QStringLiteral("Bearer ") + token},
        {"trakt-api-version", "2"},
        {"trakt-api-key", clientId()},
    };
    Client session = client->withSession(QStringLiteral("trakt"));
    const int wanted = remoteId.toInt();

    for (const QJsonValue &value : session.get(QString::fromLatin1(kApi)
                                               + QStringLiteral("/sync/watched/shows"),
                                               auth).toJsonArray()) {
        const QJsonObject row = value.toObject();
        if (row.value("show").toObject().value("ids").toObject().value("trakt").toInt() != wanted)
            continue;
        // plays counts rewatches; completed episodes is what progress means.
        int seen = 0;
        for (const QJsonValue &season : row.value("seasons").toArray())
            seen += season.toObject().value("episodes").toArray().size();
        entry.progress = seen;
        entry.status = QStringLiteral("watching");
        entry.updatedAt = QDateTime::fromString(row.value("last_watched_at").toString(),
                                                Qt::ISODate).toSecsSinceEpoch();
        entry.valid = true;
        break;
    }

    if (!entry.valid) {
        for (const QJsonValue &value : session.get(QString::fromLatin1(kApi)
                                                   + QStringLiteral("/sync/watchlist/shows"),
                                                   auth).toJsonArray()) {
            const QJsonObject show = value.toObject().value("show").toObject();
            if (show.value("ids").toObject().value("trakt").toInt() != wanted) continue;
            entry.status = QStringLiteral("watchlist");
            entry.valid = true;
            break;
        }
    }

    for (const QJsonValue &value : session.get(QString::fromLatin1(kApi)
                                               + QStringLiteral("/sync/ratings/shows"),
                                               auth).toJsonArray()) {
        const QJsonObject row = value.toObject();
        if (row.value("show").toObject().value("ids").toObject().value("trakt").toInt() != wanted)
            continue;
        entry.score = row.value("rating").toDouble();
        entry.valid = true;
        break;
    }
    return entry;
}

bool Trakt::update(Client *client, const Entry &entry) {
    const QString token = freshToken(client);
    if (token.isEmpty()) return false;
    const QMap<QString, QString> headers{
        {"Content-Type", "application/json"},
        {"Authorization", QStringLiteral("Bearer ") + token},
        {"trakt-api-version", "2"},
        {"trakt-api-key", clientId()},
    };
    Client session = client->withSession(QStringLiteral("trakt"));

    // Watchlist and history are different endpoints, and there is no "set progress" call.
    const bool queued = entry.status == QLatin1String("watchlist");
    const QByteArray body = QStringLiteral(R"({"shows":[{"ids":{"trakt":%1}}]})")
                                .arg(entry.remoteId).toUtf8();
    const auto response = session.post(
        QString::fromLatin1(kApi) + (queued ? QStringLiteral("/sync/watchlist")
                                            : QStringLiteral("/sync/history")),
        body, headers);
    bool ok = response.code >= 200 && response.code < 300;

    if (entry.score > 0) {
        const QByteArray rating = QStringLiteral(R"({"shows":[{"rating":%1,"ids":{"trakt":%2}}]})")
                                      .arg(int(qRound(entry.score))).arg(entry.remoteId).toUtf8();
        const auto rated = session.post(QString::fromLatin1(kApi) + QStringLiteral("/sync/ratings"),
                                        rating, headers);
        ok = ok && rated.code >= 200 && rated.code < 300;
    }
    return ok;
}
