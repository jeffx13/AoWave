#include "providers/iyf.h"
#include <QCryptographicHash>
#include <QDate>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include <optional>
#include "core/danmakuoptions.h"
#include "core/settings.h"
#include "core/exception.h"

Iyf::Iyf(QObject *parent) : ShowProvider(parent) {
    const Settings &settings = Settings::instance();
    m_expire = settings.value(QStringLiteral("iyf/auth/expire")).toString();
    m_sign   = settings.value(QStringLiteral("iyf/auth/sign")).toString();
    m_token  = settings.value(QStringLiteral("iyf/auth/token")).toString();
    m_uid    = settings.value(QStringLiteral("iyf/auth/uid")).toString();
}

QString Iyf::session() const {
    return QStringLiteral("uid=%1&expire=%2&gid=1&sign=%3&token=%4")
        .arg(m_uid, m_expire, m_sign, m_token);
}

QList<ShowData> Iyf::search(Client *client, const QString &query, int page, int /*typeIndex*/) {
    const QString tag = QString::fromUtf8(QUrl::toPercentEncoding(query.toLower()));
    const QString url = QStringLiteral("https://rankv21.iyf.tv/v3/list/briefsearch"
                                       "?tags=%1&orderby=4&page=%2&size=36&desc=1&isserial=-1&%3")
                            .arg(tag, QString::number(page), session());
    const KeyPair keys = authKeys(client);
    const auto results = client->post(url, {{"tag", tag},
                                            {"vv", sign("tags=" + tag, keys)},
                                            {"pub", keys.first}}, m_headers)
                             .toJsonObject()["data"].toObject()["info"].toArray()
                             .at(0).toObject()["result"].toArray();

    QList<ShowData> shows;
    shows.reserve(results.size());
    for (const QJsonValue &value : results) {
        const QJsonObject show = value.toObject();
        shows.emplaceBack(show["title"].toString(), show["contxt"].toString(),
                          show["imgPath"].toString(), this);
    }
    return shows;
}

QList<ShowData> Iyf::browse(Client *client, int page, bool latest, int typeIndex,
                            const QVariantMap &filters) {
    const int i = qBound(0, typeIndex, int(std::size(kCategoryIds)) - 1);
    // A genre is its category's id with one more part.
    const QString genre = filters.value("genre").toString();
    QString params = QStringLiteral("cinema=1&page=%1&size=36&orderby=%2&desc=1&cid=%3")
                         .arg(QString::number(page), latest ? "1" : "2",
                              genre.isEmpty() ? QLatin1String(kCategoryIds[i]) : genre);
    if (const QString year = filters.value("year").toString(); !year.isEmpty())
        params += "&year=" + year;
    const QString status = filters.value("status").toString();
    params += QStringLiteral("&isserial=%1").arg(status == "airing" ? "1" : status == "finished" ? "0" : "-1");
    const auto results = callApi(client, "https://m10.iyf.tv/api/list/Search?",
                                 params + "&isIndex=-1&isfree=-1")["result"].toArray();

    QList<ShowData> shows;
    shows.reserve(results.size());
    for (const QJsonValue &value : results) {
        const QJsonObject show = value.toObject();
        shows.emplaceBack(show["title"].toString(), show["key"].toString(),
                          show["image"].toString(), this, "", kShowTypes[i]);
    }
    return shows;
}

int Iyf::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    const QJsonObject info = callApi(
        client, "https://m10.iyf.tv/v3/video/detail?",
        QStringLiteral("cinema=1&device=1&player=CkPlayer&tech=HLS&country=HU&lang=cns&v=1&id=%1&region=UK")
            .arg(show.link));
    if (info.isEmpty()) return 0;

    QString params = QStringLiteral("cinema=1&vid=%1&lsk=1&taxis=0&cid=%2&%3")
                         .arg(show.link, info["cid"].toString(), session());
    const KeyPair keys = authKeys(client);
    const QString vv = sign(params, keys);
    params.replace(",", "%2C");

    const auto episodes = client->get("https://m10.iyf.tv/v3/video/languagesplaylist?" + params
                                      + "&vv=" + vv + "&pub=" + keys.first)
                              .toJsonObject()["data"].toObject()["info"].toArray()
                              .at(0).toObject()["playList"].toArray();
    if (episodes.isEmpty()) return 0;
    if (parts.testFlag(CountOnly)) return episodes.size();

    if (parts.testFlag(Episodes)) {
        for (const QJsonValue &value : episodes) {
            const QJsonObject episode = value.toObject();
            show.addNumberedEpisode(0, episode["key"].toString(), episode["name"].toString());
        }
    }

    if (parts.testFlag(Details)) {
        show.description = info["contxt"].toString();
        show.status      = info["lastName"].toString();
        show.views       = QString::number(info["view"].toInt(-1));
        show.updateTime  = info["updateweekly"].toString();
        show.score       = info["score"].toString();
        show.releaseDate = info["add_date"].toString();
        show.genres.push_back(info["videoType"].toString());
    }
    return episodes.size();
}

PlayInfo Iyf::extractSource(Client *client, VideoServer server) {
    PlayInfo playInfo;
    const QJsonObject response = callApi(
        client, "https://m10.iyf.tv/v3/video/play?",
        QStringLiteral("cinema=1&id=%1&a=0&lang=none&usersign=1&region=UK&device=1&isMasterSupport=0&%2")
            .arg(server.link, session()));
    if (response.isEmpty()) return playInfo;

    for (const QJsonValue &value : response["clarity"].toArray()) {
        const QJsonObject clarity = value.toObject();
        if (clarity["path"].isNull()) continue;
        const QJsonObject path = clarity["path"].toObject();
        QString source = path["result"].toString();

        if (path["needSign"].toBool() || source.startsWith("https://hss5")) {
            const KeyPair keys = authKeys(client);
            source += QStringLiteral("&vv=%1&pub=%2").arg(sign(session(), keys), keys.first);
        }
        // "title" is the height ("480") and "description" the site's name for it ("流畅");
        // "bitrate" is not one (480000 for 480p).
        playInfo.videos.emplaceBack(source, clarity["description"].toString(),
                                    clarity["title"].toString().toInt());
        break;
    }

    // The web player asks for the comments by the play answer's uniqueKey, not the episode key.
    if (const QString uniqueKey = response["uniqueKey"].toString();
        !uniqueKey.isEmpty() && !playInfo.videos.isEmpty()) {
        playInfo.danmakuKey = QStringLiteral("iyf-%1").arg(uniqueKey);
        // Its own client, not the race's: that one is cancelled the moment a server wins.
        playInfo.danmakuSource = [uniqueKey, keys = authKeys(client),
                                  started = std::make_shared<std::optional<QFuture<QList<DanmakuComment>>>>()]() {
            if (!*started)
                *started = QtConcurrent::run([uniqueKey, keys]() {
                    Client worker({}, false);
                    return Iyf::fetchDanmaku(&worker, uniqueKey, keys);
                });
            return **started;
        };
        if (DanmakuOptions::current().enabled) playInfo.danmakuSource();
    }
    return playInfo;
}

QList<DanmakuComment> Iyf::fetchDanmaku(Client *client, const QString &uniqueKey, const KeyPair &keys) {
    // One page of 12000, as the web player asks for.
    const QString query = QStringLiteral("cinema=1&page=1&size=12000&uniqueKey=%1").arg(uniqueKey);
    const QJsonArray items =
        client->get(QStringLiteral("https://m10.iyf.tv/api/video/getBarrage?") + query
                        + QStringLiteral("&vv=") + sign(query, keys) + QStringLiteral("&pub=") + keys.first,
                    {{"referer", "https://www.iyf.tv"}, {"X-Requested-With", "XMLHttpRequest"}})
            .toJsonObject()["data"].toObject()["info"].toArray();

    QList<DanmakuComment> comments;
    comments.reserve(items.size());
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (item["isDeleted"].toInt() != 0) continue;
        DanmakuComment comment;
        comment.text = item["contxt"].toString().trimmed();
        if (comment.text.isEmpty()) continue;
        comment.timeMs = int(item["second"].toDouble() * 1000);
        // The site's picker: 0 scrolls, 1 is pinned to the top, 2 to the bottom.
        const int position = item["position"].toInt();
        comment.mode = position == 1 ? 5 : position == 2 ? 4 : 1;
        bool ok = false;
        const quint32 rgb = item["color"].toString().remove(QLatin1Char('#')).toUInt(&ok, 16);
        if (ok) comment.color = rgb & 0xFFFFFFu;
        comments.append(comment);
    }
    return comments;
}

QVariantMap Iyf::filterOptions(Client *client, int typeIndex) {
    const int i = qBound(0, typeIndex, int(std::size(kCategoryIds)) - 1);
    const KeyPair keys = authKeys(client);
    const QString query = QStringLiteral("cinema=1");
    const QJsonArray list = client->get(QStringLiteral("https://m10.iyf.tv/api/list/%1?%2&vv=%3&pub=%4")
                                            .arg(QLatin1String(kGenreLists[i]), query, sign(query, keys), keys.first))
                                .toJsonObject()["data"].toObject()["info"].toArray();
    QVariantList genres, years;
    for (const QJsonValue &genre : list)
        genres.append(filterOption(genre["path"].toString(), genre["className"].toString()));
    for (int year = QDate::currentDate().year(); year >= 2000; --year)
        years.append(filterOption(QString::number(year), QString::number(year)));
    QVariantMap options{{"genre", genres}, {"year", years}};
    // Only a series is still airing or complete.
    if (kShowTypes[i] != ShowData::Movie) options.insert("status", statusOptions({"airing", "finished"}));
    return options;
}

QJsonObject Iyf::callApi(Client *client, const QString &prefixUrl, const QString &query) const {
    const KeyPair keys = authKeys(client);
    const QString url = prefixUrl + query + "&vv=" + sign(query, keys) + "&pub=" + keys.first;
    return client->get(url).toJsonObject()["data"].toObject()["info"].toArray().at(0).toObject();
}

// By value under the lock: a reference to the shared map races.
Iyf::KeyPair Iyf::authKeys(Client *client) const {
    static QMutex mutex;
    static KeyPair keys;
    QMutexLocker locker(&mutex);
    if (keys.first.isEmpty()) {
        static const QRegularExpression pattern(
            R"("publicKey":"([^"]+)\","privateKey\":\[\"([^"]+)\")");
        const auto match = pattern.match(client->get(hostUrl()).body);
        if (!match.hasMatch() || match.lastCapturedIndex() != 2)
            throw AppException(tr("Could not read the keys iyf signs its requests with."), name());
        keys = {match.captured(1), match.captured(2)};
    }
    return keys;
}

QString Iyf::sign(const QString &input, const KeyPair &keys) {
    const auto &[publicKey, privateKey] = keys;
    return QCryptographicHash::hash((publicKey + "&" + input.toLower() + "&" + privateKey).toUtf8(),
                                    QCryptographicHash::Md5).toHex();
}
