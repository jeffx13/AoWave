#include "providers/olevod.h"
#include "core/exception.h"
#include "net/hlsproxy.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QUrl>

QString Olevod::vv(qint64 unixSeconds) {
    const QString ts = QString::number(unixSeconds);
    QString bits[4];
    for (const QChar &c : ts) {
        const QString b = QString::number(c.unicode(), 2);
        for (int i = 0; i < 4; ++i)
            bits[i] += (i < 3) ? b.mid(2 + i, 1) : b.mid(5);
    }
    QString groups[4];
    for (int i = 0; i < 4; ++i) {
        const QString hex = bits[i].isEmpty() ? QString()
                                              : QString::number(bits[i].toULongLong(nullptr, 2), 16);
        groups[i] = hex.isEmpty() ? QStringLiteral("000") : hex.rightJustified(3, '0').left(3);
    }
    const QString n = QString::fromLatin1(
        QCryptographicHash::hash(ts.toUtf8(), QCryptographicHash::Md5).toHex());
    return n.mid(0, 3) + groups[0] + n.mid(6, 5) + groups[1] + n.mid(14, 5) + groups[2]
         + n.mid(22, 5) + groups[3] + n.mid(30);
}

QString Olevod::signedUrl(const QString &path) {
    return QString("%1%2?_vv=%3").arg(kApi, path, vv(QDateTime::currentSecsSinceEpoch()));
}

QList<ShowData> Olevod::parseList(const QJsonArray &items) {
    QList<ShowData> shows;
    shows.reserve(items.size());
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        const QString id = QString::number(o.value("id").toInt());
        const QString title = o.value("name").toString();
        if (id == "0" || title.isEmpty()) continue;
        QString cover = o.value("picThumb").toString();
        if (cover.isEmpty()) cover = o.value("pic").toString();
        if (!cover.isEmpty() && !cover.startsWith("http")) cover = kStatic + cover;
        shows.emplaceBack(title, id, cover, this, o.value("remarks").toString());
    }
    return shows;
}

QList<ShowData> Olevod::listing(Client *client, int page, int typeIndex, const QString &sort,
                                const QVariantMap &filters) {
    const int typeId1 = kTypeIds[qBound(0, typeIndex, int(std::size(kTypeIds)) - 1)];
    // .../type/genre/year/sort/page/size; 0 is any.
    const QString genre = filters.value("genre").toString(), year = filters.value("year").toString();
    const QString path = QString("/v1/pub/vod/list/true/3/0/0/%1/%2/%3/%4/%5/%6")
                             .arg(typeId1).arg(genre.isEmpty() ? QStringLiteral("0") : genre)
                             .arg(year.isEmpty() ? QStringLiteral("0") : year)
                             .arg(sort).arg(qMax(1, page)).arg(kPageSize);
    return parseList(client->get(signedUrl(path), m_headers)
                         .toJsonObject().value("data").toObject()
                         .value("list").toArray());
}

QList<ShowData> Olevod::search(Client *client, const QString &query, int page, int /*typeIndex*/) {
    if (query.trimmed().isEmpty()) return {};
    const QString path = QString("/v1/pub/index/search/%1/0/0/%2/0")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(query)))
                             .arg(qMax(1, page));
    const QJsonArray buckets = client->get(signedUrl(path), m_headers)
                                   .toJsonObject().value("data").toObject()
                                   .value("data").toArray();
    for (const QJsonValue &b : buckets) {
        const QJsonObject o = b.toObject();
        if (o.value("type").toString() == "vod")
            return parseList(o.value("list").toArray());
    }
    return {};
}

QList<ShowData> Olevod::popular(Client *client, int page, int typeIndex) {
    return listing(client, page, typeIndex, QStringLiteral("hot"));
}

QList<ShowData> Olevod::latest(Client *client, int page, int typeIndex) {
    return listing(client, page, typeIndex, QStringLiteral("update"));
}

QVariantMap Olevod::filterOptions(Client *client, int typeIndex) {
    const int typeId1 = kTypeIds[qBound(0, typeIndex, int(std::size(kTypeIds)) - 1)];
    const QJsonArray types = client->get(signedUrl("/v1/pub/vod/list/type"), m_headers)
                                 .toJsonObject().value("data").toArray();
    for (const QJsonValue &entry : types) {
        const QJsonObject type = entry.toObject();
        if (type.value("typeId").toInt() != typeId1) continue;
        QVariantList genres, years;
        for (const QJsonValue &child : type.value("children").toArray())
            genres.append(filterOption(QString::number(child["typeId"].toInt()), child["typeName"].toString()));
        for (const QJsonValue &year : type.value("year").toArray())
            years.append(filterOption(year.toString(), year.toString()));
        return {{"genre", genres}, {"year", years}};
    }
    return {};
}

QList<ShowData> Olevod::filtered(Client *client, const QString & /*query*/, int page, int typeIndex,
                                 const QVariantMap &filters, bool latest) {
    return listing(client, page, typeIndex, latest ? QStringLiteral("update") : QStringLiteral("hot"), filters);
}

int Olevod::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    const QString path = QString("/v1/pub/vod/detail/%1/true").arg(show.link);
    const QJsonObject data = client->get(signedUrl(path), m_headers)
                                 .toJsonObject().value("data").toObject();
    if (data.isEmpty()) return 0;

    // VIP-only entries come back with an empty url.
    const QJsonArray urls = data.value("urls").toArray();
    int playable = 0;
    for (const QJsonValue &v : urls)
        if (!v.toObject().value("url").toString().isEmpty()) ++playable;
    if (parts.testFlag(CountOnly)) return playable;
    if (playable == 0 && !urls.isEmpty())
        throw AppException(tr("%1 is only available to OleVod VIP members.")
                               .arg(data.value("name").toString(show.title)), name());

    if (parts.testFlag(Episodes)) {
        for (const QJsonValue &v : urls) {
            const QJsonObject e = v.toObject();
            const QString link = e.value("url").toString();
            if (link.isEmpty()) continue;
            show.addNumberedEpisode(0, link, e.value("title").toString());
        }
    }

    if (parts.testFlag(Details)) {
        show.description = data.value("content").toString();
        show.releaseDate = data.value("year").toVariant().toString();
        show.status      = data.value("remarks").toString();
        if (const qint64 stamp = data.value("vodTime").toVariant().toLongLong(); stamp > 0) {
            const auto time = stamp > 100000000000LL ? QDateTime::fromMSecsSinceEpoch(stamp)
                                                     : QDateTime::fromSecsSinceEpoch(stamp);
            show.updateTime = QLocale::system().toString(time, QLocale::ShortFormat);
        }
        show.score       = QString::number(data.value("score").toDouble());
        show.views       = QLocale::system().toString(data.value("hits").toInteger());
        for (const char *key : {"typeIdName", "area", "lang"}) {
            const QString g = data.value(key).toString();
            if (!g.isEmpty()) show.genres.push_back(g);
        }
    }
    return playable;
}

QList<VideoServer> Olevod::loadServers(Client * /*client*/, const PlaylistItem *episode) const {
    return { VideoServer("OleVod", episode->link) };
}

PlayInfo Olevod::extractSource(Client * /*client*/, VideoServer server) {
    PlayInfo info;
    if (server.link.isEmpty()) return info;
    // Through the proxy: its Client falls back to DNS over HTTPS where the CDN host does not
    // resolve, which mpv cannot.
    const QString url = HlsProxy::instance() ? HlsProxy::instance()->playlistUrl(server.link, hostUrl())
                                             : server.link;
    info.videos.emplaceBack(QUrl(url));
    info.addHeader("Referer", hostUrl());
    info.addHeader("Origin", "https://www.olevod.com");
    info.addHeader("User-Agent", m_headers.value("User-Agent"));
    return info;
}
