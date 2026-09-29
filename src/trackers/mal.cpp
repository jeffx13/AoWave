#include "trackers/mal.h"
#include "core/logger.h"
#include "core/settings.h"
#include "core/appshell.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace {
constexpr const char *kApi = "https://api.myanimelist.net/v2";

QString randomVerifier() {
    // 64 unreserved characters, inside MAL's 43..128 range.
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";
    QString out;
    for (int i = 0; i < 64; ++i)
        out.append(QLatin1Char(alphabet[QRandomGenerator::global()->bounded(int(sizeof(alphabet)) - 1)]));
    return out;
}
}

Mal::Mal(QObject *parent) : Tracker(parent) { loadToken(); }

void Mal::authenticate() {
    if (!canAuthenticate()) {
        AppShell::instance().reportError(registrationHint(), name());
        return;
    }
    // MAL supports PKCE only with code_challenge_method=plain; S256 fails as invalid_request.
    m_verifier = randomVerifier();

    LoopbackAuth *loopback = LoopbackAuth::start(this, m_pendingAuth, kRedirectPort, name());
    if (!loopback) return;

    connect(loopback, &LoopbackAuth::codeReceived, this, [this, loopback](const QString &code) {
        loopback->deleteLater();
        const QString verifier = m_verifier;
        runInBackground([this, code, verifier, id = clientId()](const CancelToken &cancel) {
            Client client(cancel);
            QMap<QString, QString> form{
                {"client_id", id},
                {"grant_type", "authorization_code"},
                {"code", code},
                {"code_verifier", verifier},
                {"redirect_uri", QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort)},
            };
            const auto response = client.post(QStringLiteral("https://myanimelist.net/v1/oauth2/token"),
                                              form, {{"Content-Type", "application/x-www-form-urlencoded"}});
            const QJsonObject json = response.toJsonObject();
            const QString token = json.value("access_token").toString();
            const QString refresh = json.value("refresh_token").toString();
            const qint64 expires = json.value("expires_in").toInt();
            if (token.isEmpty()) {
                const QString why = json.value("message").toString(json.value("error").toString());
                QMetaObject::invokeMethod(this, [this, why, status = response.code]() {
                    logWarn() << name() << "token exchange failed:" << status << why;
                    AppShell::instance().reportError(
                        tr("MyAnimeList refused the sign-in%1\n\n%2")
                            .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why,
                                 registrationHint()),
                        name());
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(this, [this, token, refresh, expires]() {
                storeToken({token, refresh, QDateTime::currentSecsSinceEpoch() + expires});
                fetchViewer();
            }, Qt::QueuedConnection);
        });
    });
    QDesktopServices::openUrl(QUrl(
        QStringLiteral("https://myanimelist.net/v1/oauth2/authorize?response_type=code"
                       "&client_id=%1&code_challenge=%2&code_challenge_method=plain"
                       "&redirect_uri=http://127.0.0.1:%3&state=%4")
            .arg(clientId(), m_verifier).arg(kRedirectPort).arg(loopback->state())));
}

void Mal::fetchViewer() {
    runInBackground([this, token = accessToken()](const CancelToken &cancel) {
        Client client(cancel);
        const auto response = client.get(QString::fromLatin1(kApi) + QStringLiteral("/users/@me"),
                                         {{"Authorization", QStringLiteral("Bearer ") + token}},
                                         {{"fields", "name"}});
        const QString name = response.toJsonObject().value("name").toString();
        if (name.isEmpty()) return;
        QMetaObject::invokeMethod(this, [this, name]() { setAccountName(name); },
                                  Qt::QueuedConnection);
    });
}

// MAL tokens last about a month and it always issues a refresh token.
Tracker::Grant Mal::refreshGrant(Client *client, const QString &refreshToken) {
    const QMap<QString, QString> form{
        {"client_id", clientId()},
        {"grant_type", "refresh_token"},
        {"refresh_token", refreshToken},
    };
    const auto response = client->post(QStringLiteral("https://myanimelist.net/v1/oauth2/token"),
                                       form, {{"Content-Type", "application/x-www-form-urlencoded"}});
    const QJsonObject json = response.toJsonObject();
    return {json.value("access_token").toString(), json.value("refresh_token").toString(),
            QDateTime::currentSecsSinceEpoch() + json.value("expires_in").toInt()};
}

QList<Tracker::Result> Mal::search(Client *client, const QString &query) {
    const QString token = freshToken(client);
    if (token.isEmpty()) return {};
    const auto response = client->withSession(QStringLiteral("mal"))
        .get(QString::fromLatin1(kApi) + QStringLiteral("/anime"),
             {{"Authorization", QStringLiteral("Bearer ") + token}},
             {{"q", query}, {"limit", "20"}, {"fields", "num_episodes,start_season,main_picture"}});

    QList<Result> results;
    for (const QJsonValue &value : response.toJsonObject().value("data").toArray()) {
        const QJsonObject node = value.toObject().value("node").toObject();
        Result result;
        result.remoteId = QString::number(node.value("id").toInt());
        result.title = node.value("title").toString();
        result.cover = node.value("main_picture").toObject().value("medium").toString();
        result.episodes = node.value("num_episodes").toInt();
        const int year = node.value("start_season").toObject().value("year").toInt();
        if (year > 0) result.year = QString::number(year);
        results.append(result);
    }
    return results;
}

Tracker::Entry Mal::entryFor(Client *client, const QString &remoteId) {
    Entry entry;
    entry.remoteId = remoteId;
    const QString token = freshToken(client);
    if (token.isEmpty()) return entry;
    const auto response = client->withSession(QStringLiteral("mal"))
        .get(QString::fromLatin1(kApi) + QStringLiteral("/anime/") + remoteId,
             {{"Authorization", QStringLiteral("Bearer ") + token}},
             {{"fields", "my_list_status"}});
    const QJsonObject status = response.toJsonObject().value("my_list_status").toObject();
    if (status.isEmpty()) return entry;
    entry.status = status.value("status").toString();
    entry.score = status.value("score").toDouble();
    entry.progress = status.value("num_episodes_watched").toInt();
    entry.valid = true;
    return entry;
}

bool Mal::update(Client *client, const Entry &entry) {
    const QString token = freshToken(client);
    if (token.isEmpty()) return false;
    QMap<QString, QString> form{
        {"status", entry.status},
        {"score", QString::number(int(entry.score))},
        {"num_watched_episodes", QString::number(entry.progress)},
    };
    const auto response = client->withSession(QStringLiteral("mal"))
        .post(QString::fromLatin1(kApi) + QStringLiteral("/anime/") + entry.remoteId
                  + QStringLiteral("/my_list_status"), form,
              {{"Authorization", QStringLiteral("Bearer ") + token},
               {"Content-Type", "application/x-www-form-urlencoded"}});
    return response.code >= 200 && response.code < 300;
}
