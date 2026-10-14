#include "library/trackers/tracker.h"
#include "app/logger.h"
#include "app/settings.h"
#include "app/mainwindow.h"
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

QJsonObject AniList::graphql(Client *client, const QString &query, const QJsonObject &variables,
                             QString *error) {
    QMap<QString, QString> headers{
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
    };
    if (const QString token = freshToken(client); !token.isEmpty())
        headers["Authorization"] = QStringLiteral("Bearer ") + token;

    const QByteArray body = QJsonDocument(QJsonObject{
        {"query", query}, {"variables", variables}}).toJson(QJsonDocument::Compact);
    Client session = client->withSession(QStringLiteral("anilist"));
    const auto response = session.post(QString::fromLatin1(kApi), body, headers);
    const QJsonObject json = QJsonDocument::fromJson(
        (response.body.isEmpty() ? response.errorBody : response.body).toUtf8()).object();
    QStringList messages;
    for (const QJsonValue &e : json.value("errors").toArray())
        messages << e.toObject().value("message").toString();
    messages.removeAll(QString());
    if (messages.isEmpty() && response.code != 200)
        messages << (session.lastError().isEmpty() ? tr("AniList did not respond.") : session.lastError());
    if (!messages.isEmpty()) logWarn() << "Sync" << name() + QLatin1Char(':') << messages.join(QStringLiteral("; "));
    if (error) *error = messages.join(QStringLiteral("; "));
    return json.value("data").toObject();
}

void AniList::authenticate() {
    if (clientId().isEmpty()) {
        MainWindow::instance().reportError(registrationHint(), name());
        return;
    }
    bool numeric = false;
    clientId().toInt(&numeric);
    if (!numeric) {
        MainWindow::instance().reportError(
            tr("That is the Client Secret, not the Client ID. The ID is the number above it on "
               "AniList's developer page."),
            name());
        return;
    }
    if (clientSecret().isEmpty()) {
        openAuthorization();
        return;
    }
    // A wrong Redirect URL or Secret shows only on AniList's page as invalid_client; a token request with a made-up code tells first.
    runInBackground([this, id = clientId(), secret = clientSecret()](const CancelToken &cancel) {
        Client client(cancel, false);
        enum class Probe { Refused, Answered, NoAnswer };
        const auto probe = [&](bool withRedirect) {
            QJsonObject body{{"grant_type", "authorization_code"}, {"client_id", id},
                             {"client_secret", secret}, {"code", "check"}};
            if (withRedirect) body["redirect_uri"] = QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort);
            const auto response = client.post(QStringLiteral("https://anilist.co/api/v2/oauth/token"),
                                              QJsonDocument(body).toJson(QJsonDocument::Compact),
                                              {{"Content-Type", "application/json"}, {"Accept", "application/json"}});
            const QString text = response.body.isEmpty() ? response.errorBody : response.body;
            const QJsonObject json = QJsonDocument::fromJson(text.toUtf8()).object();
            const QString error = json.value("error").toString();
            if (error == QLatin1String("invalid_client")) return Probe::Refused;
            return error.isEmpty() && !json.contains("access_token") ? Probe::NoAnswer : Probe::Answered;
        };
        QString problem;
        if (probe(true) == Probe::Refused)
            problem = probe(false) != Probe::Answered
                ? tr("AniList does not recognise this Client ID and Client Secret. Copy both again from AniList's "
                     "developer page, or clear the Secret.")
                : tr("AniList refused the redirect. Set the client's Redirect URL on AniList to exactly "
                     "http://127.0.0.1:%1 (no trailing slash), then sign in again.").arg(kRedirectPort);
        if (cancel.isCancelled()) return;
        QMetaObject::invokeMethod(this, [this, problem]() {
            if (!problem.isEmpty()) {
                logWarn() << "Sync" << name() + QLatin1Char(':') << problem;
                MainWindow::instance().reportError(problem, name());
                return;
            }
            openAuthorization();
        }, Qt::QueuedConnection);
    });
}

void AniList::openAuthorization() {
    LoopbackAuth *loopback = LoopbackAuth::start(this, m_pendingAuth, kRedirectPort, name());
    if (!loopback) return;

    const bool confidential = !clientSecret().isEmpty();

    connect(loopback, &LoopbackAuth::codeReceived, this, [this, loopback](const QString &code) {
        loopback->deleteLater();
        exchangeCode(code);
    });
    connect(loopback, &LoopbackAuth::tokenReceived, this,
            [this, loopback](const QString &token, qint64 expiresIn) {
                loopback->deleteLater();
                storeToken({token, {}, expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0});
                fetchViewer();
            });

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
                {"client_secret", secret},
                {"redirect_uri", QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort)},
                {"code", code},
            }).toJson(QJsonDocument::Compact);
            const auto response = client.post(
                QStringLiteral("https://anilist.co/api/v2/oauth/token"), body,
                {{"Content-Type", "application/json"}, {"Accept", "application/json"}});

            const QJsonObject json = QJsonDocument::fromJson(
                (response.body.isEmpty() ? response.errorBody : response.body).toUtf8()).object();
            const QString token = json.value("access_token").toString();
            if (token.isEmpty()) {
                const QString error = json.value("error").toString();
                QString why = json.value("message").toString(
                    json.value("hint").toString(error));
                if (error == QLatin1String("invalid_client"))
                    why = QStringLiteral("AniList did not recognise the Client ID and Secret "
                                         "together. Clear the Client Secret box and sign in "
                                         "again - AniList does not need one here.");
                QMetaObject::invokeMethod(this, [this, why, status = response.code]() {
                    logWarn() << "Sync" << name() + QLatin1Char(':') << "token exchange failed:" << status << why;
                    MainWindow::instance().reportError(
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
                logWarn() << "Sync" << name() + QLatin1Char(':') << "signed in, but the viewer query came back empty";
                MainWindow::instance().reportError(
                    tr("Could not read your AniList account. Check that the client's Redirect URL is "
                       "http://127.0.0.1:%1.")
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

namespace {
QString dateOf(const QJsonValue &fuzzy) {
    const QJsonObject o = fuzzy.toObject();
    QStringList parts;
    if (const int y = o.value("year").toInt()) {
        parts << QString::number(y);
        if (const int m = o.value("month").toInt()) {
            parts << QStringLiteral("%1").arg(m, 2, 10, QLatin1Char('0'));
            if (const int d = o.value("day").toInt()) parts << QStringLiteral("%1").arg(d, 2, 10, QLatin1Char('0'));
        }
    }
    return parts.join(QLatin1Char('-'));
}

// Null fields clear what is not given; leaving them out would keep it.
QJsonObject fuzzyOf(const QString &date) {
    const QStringList parts = date.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    const auto part = [&parts](int i) { return i < parts.size() && parts[i].toInt() > 0 ? QJsonValue(parts[i].toInt())
                                                                                     : QJsonValue(QJsonValue::Null); };
    return {{"year", part(0)}, {"month", part(1)}, {"day", part(2)}};
}
}

Tracker::Entry AniList::entryFor(Client *client, const QString &remoteId) {
    const QJsonObject data = graphql(client, QStringLiteral(
        "query($id:Int){Media(id:$id){siteUrl episodes isFavourite title{romaji english}"
        "mediaListEntry{status score progress repeat notes updatedAt "
        "startedAt{year month day} completedAt{year month day}}}}"),
        {{"id", remoteId.toInt()}});

    Entry entry;
    entry.remoteId = remoteId;
    const QJsonObject media = data.value("Media").toObject();
    if (media.isEmpty()) return entry;
    entry.valid = true;
    const QJsonObject title = media.value("title").toObject();
    entry.title = title.value("english").toString(title.value("romaji").toString());
    entry.siteUrl = media.value("siteUrl").toString();
    entry.total = media.value("episodes").toInt();
    entry.favourite = media.value("isFavourite").toBool();

    const QJsonObject list = media.value("mediaListEntry").toObject();
    if (list.isEmpty()) return entry;
    entry.onList = true;
    entry.status = list.value("status").toString();
    entry.score = list.value("score").toDouble();
    entry.progress = entry.remoteProgress = list.value("progress").toInt();
    entry.repeat = list.value("repeat").toInt();
    entry.notes = list.value("notes").toString();
    entry.started = dateOf(list.value("startedAt"));
    entry.finished = dateOf(list.value("completedAt"));
    entry.updatedAt = list.value("updatedAt").toInt();
    return entry;
}

QString AniList::update(Client *client, const Entry &entry) {
    if (accessToken().isEmpty()) return tr("Not signed in to AniList.");
    QString error;
    const QJsonObject saved = graphql(client, QStringLiteral(
        "mutation($id:Int,$status:MediaListStatus,$score:Float,$progress:Int,$repeat:Int,$notes:String,"
        "$startedAt:FuzzyDateInput,$completedAt:FuzzyDateInput){SaveMediaListEntry(mediaId:$id,"
        "status:$status,score:$score,progress:$progress,repeat:$repeat,notes:$notes,"
        "startedAt:$startedAt,completedAt:$completedAt){id}}"),
        {{"id", entry.remoteId.toInt()}, {"status", entry.status}, {"score", entry.score},
         {"progress", entry.progress}, {"repeat", entry.repeat}, {"notes", entry.notes},
         {"startedAt", fuzzyOf(entry.started)}, {"completedAt", fuzzyOf(entry.finished)}}, &error);
    if (saved.value("SaveMediaListEntry").toObject().isEmpty())
        return error.isEmpty() ? tr("AniList did not save the entry.") : error;

    const QJsonObject now = graphql(client, QStringLiteral("query($id:Int){Media(id:$id){isFavourite}}"),
                                    {{"id", entry.remoteId.toInt()}});
    const QJsonObject media = now.value("Media").toObject();
    if (!media.isEmpty() && media.value("isFavourite").toBool() != entry.favourite) {
        graphql(client, QStringLiteral("mutation($id:Int){ToggleFavourite(animeId:$id){anime{pageInfo{total}}}}"),
                {{"id", entry.remoteId.toInt()}}, &error);
        if (!error.isEmpty()) return error;
    }
    return {};
}
