#include "library/trackers/tracker.h"
#include <QJsonDocument>
#include <QGuiApplication>
#include <QClipboard>
#include "app/mainwindow.h"
#include "app/logger.h"
#include "app/settings.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>
#include <QUrl>

namespace {
constexpr const char *kApi = "https://api.trakt.tv";

QMap<QString, QString> apiHeaders(const QString &clientId, const QString &token = {}) {
    QMap<QString, QString> headers{{"trakt-api-version", "2"}, {"trakt-api-key", clientId}};
    if (!token.isEmpty()) headers.insert("Authorization", QStringLiteral("Bearer ") + token);
    return headers;
}

const QMap<QString, QString> kJson{{"Content-Type", "application/json"}};

QByteArray json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
}

Trakt::PollOutcome Trakt::pollOutcome(int code, bool hasToken) {
    if (code == 200 && hasToken) return PollOutcome::SignedIn;
    if (code == 429) return PollOutcome::SlowDown;
    if (code == 400 || code <= 0 || code >= 500) return PollOutcome::Pending;
    return PollOutcome::Refused;
}

Trakt::Trakt(QObject *parent) : Tracker(parent) { loadToken(); }

void Trakt::authenticate() {
    if (!canAuthenticate()) {
        MainWindow::instance().reportError(registrationHint(), name());
        return;
    }
    runInBackground([this, id = clientId()](const CancelToken &cancel) {
        Client client(cancel);
        const auto response = client.post(QString::fromLatin1(kApi) + QStringLiteral("/oauth/device/code"),
                                          json({{"client_id", id}}), kJson);
        const QJsonObject json = response.toJsonObject();
        const QString deviceCode = json.value("device_code").toString();
        const QString userCode = json.value("user_code").toString();
        const QString url = json.value("verification_url").toString();
        const int interval = qMax(1, json.value("interval").toInt(5));
        const int expires = json.value("expires_in").toInt(600);
        if (deviceCode.isEmpty()) {
            if (cancel.isCancelled()) return;
            logWarn() << "Sync" << QStringLiteral("Trakt:") << "device code request failed:" << response.code << response.error;
            MainWindow::instance().reportError(
                tr("Could not start the Trakt sign-in (HTTP %1). Check the Client ID.")
                    .arg(response.code), QStringLiteral("Trakt"));
            return;
        }

        QMetaObject::invokeMethod(this, [this, deviceCode, userCode, url, interval, expires]() {
            QGuiApplication::clipboard()->setText(userCode);
            MainWindow::instance().reportInfo(
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
    m_polling = false;
    const qint64 deadline = QDateTime::currentSecsSinceEpoch() + expiresIn;

    disconnect(&m_poll, nullptr, this, nullptr);
    connect(&m_poll, &QTimer::timeout, this, [this, deviceCode, deadline, intervalSecs]() {
        if (QDateTime::currentSecsSinceEpoch() > deadline) {
            m_poll.stop();
            logWarn() << "Sync" << QStringLiteral("Trakt:") << "sign-in expired";
            MainWindow::instance().reportError(
                tr("The Trakt code expired. Sign in again."),
                QStringLiteral("Trakt"));
            return;
        }
        // A late answer to an earlier poll would come after the token was used.
        if (std::exchange(m_polling, true)) return;
        runInBackground(
            [this, deviceCode, intervalSecs, id = clientId(), secret = clientSecret()](const CancelToken &cancel) {
            Client client(cancel);
            const auto response = client.post(
                QString::fromLatin1(kApi) + QStringLiteral("/oauth/device/token"),
                json({{"code", deviceCode}, {"client_id", id}, {"client_secret", secret}}), kJson);
            const QJsonObject answer = response.toJsonObject();
            const PollOutcome outcome = pollOutcome(response.code, !answer.value("access_token").toString().isEmpty());
            QMetaObject::invokeMethod(this, [this, outcome, answer, intervalSecs]() {
                m_polling = false;
                if (!m_poll.isActive() || outcome == PollOutcome::Pending) return;
                if (outcome == PollOutcome::SlowDown) {
                    m_poll.setInterval(m_poll.interval() + intervalSecs * 1000);
                    return;
                }
                m_poll.stop();
                if (outcome == PollOutcome::Refused) {
                    logWarn() << "Sync" << QStringLiteral("Trakt:") << "sign-in refused";
                    MainWindow::instance().reportError(tr("Trakt refused the sign-in."),
                                                     QStringLiteral("Trakt"));
                    return;
                }
                storeToken({answer.value("access_token").toString(), answer.value("refresh_token").toString(),
                            QDateTime::currentSecsSinceEpoch() + answer.value("expires_in").toInt()});
                fetchViewer();
            }, Qt::QueuedConnection);
        });
    });
    m_poll.start();
}

QList<Tracker::Result> Trakt::search(Client *client, const QString &query) {
    const auto response = client->withSession(QStringLiteral("trakt"))
        .get(QString::fromLatin1(kApi) + QStringLiteral("/search/show,movie"), apiHeaders(clientId()),
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

void Trakt::fetchViewer() {
    lookUpAccount([token = accessToken(), id = clientId()](Client &client) {
        return client.get(QString::fromLatin1(kApi) + QStringLiteral("/users/settings"), apiHeaders(id, token))
            .toJsonObject().value("user").toObject().value("username").toString();
    });
}

Tracker::Grant Trakt::refreshGrant(Client *client, const QString &refreshToken) {
    const QByteArray body = json({{"refresh_token", refreshToken}, {"client_id", clientId()},
                                  {"client_secret", clientSecret()}, {"grant_type", "refresh_token"},
                                  {"redirect_uri", "urn:ietf:wg:oauth:2.0:oob"}});
    const auto response = client->post(QString::fromLatin1(kApi) + QStringLiteral("/oauth/token"), body, kJson);
    const QJsonObject json = response.toJsonObject();
    return {json.value("access_token").toString(), json.value("refresh_token").toString(),
            QDateTime::currentSecsSinceEpoch() + json.value("expires_in").toInt()};
}

Tracker::Entry Trakt::entryFor(Client *client, const QString &remoteId) {
    Entry entry;
    entry.remoteId = remoteId;
    const QString token = freshToken(client);
    if (token.isEmpty()) return entry;

    const QMap<QString, QString> auth = apiHeaders(clientId(), token);
    Client session = client->withSession(QStringLiteral("trakt"));
    const int wanted = remoteId.toInt();

    bool answered = false;
    const auto list = [&](const char *path) {
        const auto response = session.get(QString::fromLatin1(kApi) + QLatin1String(path), auth);
        answered = answered || response.code == 200;
        return response.toJsonArray();
    };
    for (const QJsonValue &value : list("/sync/watched/shows")) {
        const QJsonObject row = value.toObject();
        if (row.value("show").toObject().value("ids").toObject().value("trakt").toInt() != wanted)
            continue;
        int seen = 0;
        for (const QJsonValue &season : row.value("seasons").toArray())
            seen += season.toObject().value("episodes").toArray().size();
        entry.progress = entry.remoteProgress = seen;
        entry.status = QStringLiteral("watching");
        entry.updatedAt = QDateTime::fromString(row.value("last_watched_at").toString(),
                                                Qt::ISODate).toSecsSinceEpoch();
        entry.onList = true;
        break;
    }

    if (!entry.onList) {
        for (const QJsonValue &value : list("/sync/watchlist/shows")) {
            const QJsonObject show = value.toObject().value("show").toObject();
            if (show.value("ids").toObject().value("trakt").toInt() != wanted) continue;
            entry.status = QStringLiteral("watchlist");
            entry.onList = true;
            break;
        }
    }

    for (const QJsonValue &value : list("/sync/ratings/shows")) {
        const QJsonObject row = value.toObject();
        if (row.value("show").toObject().value("ids").toObject().value("trakt").toInt() != wanted)
            continue;
        entry.score = row.value("rating").toDouble();
        entry.onList = true;
        break;
    }
    entry.valid = answered;
    entry.siteUrl = QStringLiteral("https://trakt.tv/shows/") + remoteId;
    return entry;
}

QString Trakt::update(Client *client, const Entry &entry) {
    const QString token = freshToken(client);
    if (token.isEmpty()) return tr("Not signed in to Trakt.");
    QMap<QString, QString> headers = apiHeaders(clientId(), token);
    headers.insert(kJson);
    Client session = client->withSession(QStringLiteral("trakt"));

    // No "set progress" call: each post to history is a new play, so only episodes past Trakt's own count are sent.
    QJsonObject show{{"ids", QJsonObject{{"trakt", entry.remoteId.toInt()}}}};
    const bool queued = entry.status == QLatin1String("watchlist");
    const bool completed = entry.status == QLatin1String("completed");
    bool post = queued || completed;
    if (!post && entry.progress > entry.remoteProgress) {
        QJsonArray episodes;
        for (int n = entry.remoteProgress + 1; n <= entry.progress; ++n) episodes.append(QJsonObject{{"number", n}});
        show["seasons"] = QJsonArray{QJsonObject{{"number", 1}, {"episodes", episodes}}};
        post = true;
    }
    if (post) {
        const auto response = session.post(
            QString::fromLatin1(kApi) + (queued ? QStringLiteral("/sync/watchlist") : QStringLiteral("/sync/history")),
            json({{"shows", QJsonArray{show}}}), headers);
        if (response.code < 200 || response.code >= 300) return session.lastError();
    }

    if (entry.score > 0) {
        const QJsonObject rating{{"rating", int(qRound(entry.score))},
                                 {"ids", QJsonObject{{"trakt", entry.remoteId.toInt()}}}};
        const auto rated = session.post(QString::fromLatin1(kApi) + QStringLiteral("/sync/ratings"),
                                        json({{"shows", QJsonArray{rating}}}), headers);
        if (rated.code < 200 || rated.code >= 300) return session.lastError();
    }
    return {};
}
