#include "trackers/anilist.h"
#include "core/logger.h"
#include "core/settings.h"
#include "core/appshell.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {
constexpr const char *kApi = "https://graphql.anilist.co";

Tracker::ScoreFormat parseFormat(const QString &raw) {
    if (raw == QLatin1String("POINT_100"))        return Tracker::ScoreFormat::Point100;
    if (raw == QLatin1String("POINT_5"))          return Tracker::ScoreFormat::Point5;
    if (raw == QLatin1String("POINT_3"))          return Tracker::ScoreFormat::Point3;
    if (raw == QLatin1String("POINT_10_DECIMAL")) return Tracker::ScoreFormat::Point10Decimal;
    return Tracker::ScoreFormat::Point10;
}
}

AniList::AniList(QObject *parent) : Tracker(parent) {
    loadToken();
    m_scoreFormat = parseFormat(Settings::instance()
                                    .value(settingsGroup() + QStringLiteral("/scoreFormat")).toString());
}

QJsonObject AniList::graphql(Client *client, const QString &query, const QJsonObject &variables) {
    QMap<QString, QString> headers{
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    if (const QString token = freshToken(client); !token.isEmpty())
        headers["Authorization"] = QStringLiteral("Bearer ") + token;

    const QByteArray body = QJsonDocument(QJsonObject{
        {"query", query}, {"variables", variables}}).toJson(QJsonDocument::Compact);
    const auto response = client->withSession(QStringLiteral("anilist"))
                              .post(QString::fromLatin1(kApi), body, headers);
    return response.toJsonObject().value("data").toObject();
}

void AniList::authenticate() {
    if (clientId().isEmpty()) {
        AppShell::instance().reportError(registrationHint(), name());
        return;
    }
    LoopbackAuth *loopback = LoopbackAuth::start(this, m_pendingAuth, kRedirectPort, name());
    if (!loopback) return;

    // With a secret, AniList hands back a code to exchange; without one it puts the token
    // straight in the redirect's fragment, which the loopback's bridge page reflects back.
    const bool confidential = !clientSecret().isEmpty();

    connect(loopback, &LoopbackAuth::codeReceived, this, [this, loopback](const QString &code) {
        loopback->deleteLater();
        exchangeCode(code);
    });
    connect(loopback, &LoopbackAuth::tokenReceived, this,
            [this, loopback](const QString &token, qint64 expiresIn) {
                loopback->deleteLater();
                // AniList's implicit tokens last a year; it issues no refresh token either way.
                storeToken({token, {}, expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0});
                fetchViewer();
            });

    // The redirect URI is compared character for character, so it is sent exactly as the
    // registration hint spells it.
    QDesktopServices::openUrl(QUrl(
        QStringLiteral("https://anilist.co/api/v2/oauth/authorize?client_id=%1"
                       "&redirect_uri=http://127.0.0.1:%2&response_type=%3&state=%4")
            .arg(clientId())
            .arg(kRedirectPort)
            .arg(confidential ? QStringLiteral("code") : QStringLiteral("token"), loopback->state())));
}

void AniList::exchangeCode(const QString &code) {
    runInBackground(
        [this, code, id = clientId(), secret = clientSecret()](const CancelToken &cancel) {
            Client client(cancel);
            const QByteArray body = QJsonDocument(QJsonObject{
                {"grant_type", "authorization_code"},
                {"client_id", id},
                // Without this AniList answers invalid_client, which reads like a bad id.
                {"client_secret", secret},
                // Echoed back verbatim or the exchange is refused.
                {"redirect_uri", QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort)},
                {"code", code},
            }).toJson(QJsonDocument::Compact);
            const auto response = client.post(
                QStringLiteral("https://anilist.co/api/v2/oauth/token"), body,
                {{"Content-Type", "application/json"}, {"Accept", "application/json"}});

            const QJsonObject json = response.toJsonObject();
            const QString token = json.value("access_token").toString();
            if (token.isEmpty()) {
                const QString error = json.value("error").toString();
                QString why = json.value("message").toString(
                    json.value("hint").toString(error));
                // The one everybody hits: the Client ID pasted into the secret box, or a
                // secret from a different client. Neither is worth keeping a broken flow for.
                if (error == QLatin1String("invalid_client"))
                    why = QStringLiteral("AniList did not recognise the Client ID and Secret "
                                         "together. Clear the Client Secret box and sign in "
                                         "again - AniList does not need one here.");
                QMetaObject::invokeMethod(this, [this, why, status = response.code]() {
                    logWarn() << name() << "token exchange failed:" << status << why;
                    AppShell::instance().reportError(
                        why.isEmpty()
                            ? tr("AniList refused the sign-in (HTTP %1).").arg(status)
                            : tr("AniList refused the sign-in: %1\n\n%2")
                                  .arg(why, registrationHint()),
                        name());
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(this, [this, token]() {
                storeToken({token, {}, 0});
                fetchViewer();
            }, Qt::QueuedConnection);
        });
}

// The score scale is per-account, so it precedes any score control.
void AniList::fetchViewer() {
    runInBackground([this](const CancelToken &cancel) {
        Client client(cancel);
        const QJsonObject data = graphql(&client,
            QStringLiteral("query{Viewer{name mediaListOptions{scoreFormat}}}"), {});
        const QJsonObject viewer = data.value("Viewer").toObject();
        const QString account = viewer.value("name").toString();
        const QString format = viewer.value("mediaListOptions").toObject()
                                   .value("scoreFormat").toString();
        QMetaObject::invokeMethod(this, [this, account, format]() {
            if (account.isEmpty()) {
                logWarn() << name() << "signed in, but the viewer query came back empty";
                AppShell::instance().reportError(
                    tr("AniList accepted the sign-in but would not say who you are. "
                       "Check the client's redirect URL is http://127.0.0.1:%1.")
                        .arg(kRedirectPort),
                    name());
                return;
            }
            m_scoreFormat = parseFormat(format);
            Settings::instance().setValue(settingsGroup() + QStringLiteral("/scoreFormat"), format);
            setAccountName(account);
        }, Qt::QueuedConnection);
    });
}

QList<Tracker::Result> AniList::search(Client *client, const QString &query) {
    const QJsonObject data = graphql(client, QStringLiteral(
        "query($q:String){Page(perPage:20){media(search:$q,type:ANIME){id episodes startDate{year}"
        "title{romaji english} coverImage{medium}}}}"), {{"q", query}});

    QList<Result> results;
    for (const QJsonValue &value : data.value("Page").toObject().value("media").toArray()) {
        const QJsonObject media = value.toObject();
        const QJsonObject title = media.value("title").toObject();
        Result result;
        result.remoteId = QString::number(media.value("id").toInt());
        result.title = title.value("english").toString(title.value("romaji").toString());
        result.cover = media.value("coverImage").toObject().value("medium").toString();
        result.episodes = media.value("episodes").toInt();
        const int year = media.value("startDate").toObject().value("year").toInt();
        if (year > 0) result.year = QString::number(year);
        results.append(result);
    }
    return results;
}

Tracker::Entry AniList::entryFor(Client *client, const QString &remoteId) {
    const QJsonObject data = graphql(client, QStringLiteral(
        "query($id:Int){Media(id:$id){mediaListEntry{status score progress updatedAt}}}"),
        {{"id", remoteId.toInt()}});
    const QJsonObject list = data.value("Media").toObject().value("mediaListEntry").toObject();

    Entry entry;
    entry.remoteId = remoteId;
    if (list.isEmpty()) return entry;
    entry.status = list.value("status").toString();
    entry.score = list.value("score").toDouble();
    entry.progress = list.value("progress").toInt();
    entry.updatedAt = list.value("updatedAt").toInt();
    entry.valid = true;
    return entry;
}

bool AniList::update(Client *client, const Entry &entry) {
    if (accessToken().isEmpty()) return false;
    const QJsonObject data = graphql(client, QStringLiteral(
        "mutation($id:Int,$status:MediaListStatus,$score:Float,$progress:Int){"
        "SaveMediaListEntry(mediaId:$id,status:$status,score:$score,progress:$progress){id}}"),
        {{"id", entry.remoteId.toInt()}, {"status", entry.status},
         {"score", entry.score}, {"progress", entry.progress}});
    return !data.value("SaveMediaListEntry").toObject().isEmpty();
}
