#include "providers/anikoto.h"
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include "platform/crypto.h"
#include "net/hlsproxy.h"
#include "net/html.h"
#include "core/exception.h"

// list -> server/list -> server?get -> MegaPlay source.

namespace {

// MegaPlay returns base64url AES-256-CBC; its 16-byte key is zero-padded to 32.

QJsonObject decryptMegaPlaySource(const QString &encoded) {
    const QByteArray cipherText = QByteArray::fromBase64(encoded.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const QByteArray key = QByteArrayLiteral("i?LMTAx0Q6,:}50U") + QByteArray(16, '\0');
    const QByteArray plain = Aes::cbcDecrypt(key, QByteArrayLiteral("W0;27ToaUpl_P%'c"), cipherText);
    if (plain.isEmpty()) return {};

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(plain, &error);
    return error.error == QJsonParseError::NoError ? document.object() : QJsonObject{};
}

QString sourceUrl(const QJsonValue &value) {
    if (value.isString()) return value.toString();
    if (value.isArray()) {
        for (const QJsonValue &entry : value.toArray()) {
            const QString found = sourceUrl(entry);
            if (!found.isEmpty()) return found;
        }
        return {};
    }
    const QJsonObject object = value.toObject();
    for (const char *key : {"file", "url", "src"}) {
        const QString found = object.value(QLatin1String(key)).toString();
        if (!found.isEmpty()) return found;
    }
    for (const char *key : {"sources", "source", "links"}) {
        const QString found = sourceUrl(object.value(QLatin1String(key)));
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString addMegaPlayCdnToken(const QString &source) {
    QUrl url(source);
    if (!url.isValid()) return source;
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("token"));

    static const QRegularExpression pathKey(
        QStringLiteral(R"(/([a-f0-9]{32})/([a-f0-9]{32})/)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pathKey.match(url.path());
    if (!match.hasMatch()) return source;

    const qint64 expiry = QDateTime::currentSecsSinceEpoch() + 90;
    const QByteArray claim = QStringLiteral("%1|%2/%3")
        .arg(QString::number(expiry), match.captured(1).toLower(), match.captured(2).toLower())
        .toUtf8();
    const QByteArray signature = QMessageAuthenticationCode::hash(
        claim, QByteArrayLiteral("MpCdnT0k3n!9f2K#xQ7vL5mR8wN1pY4s"), QCryptographicHash::Sha256);
    const QByteArray token = claim.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
        + '.' + signature.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    query.addQueryItem(QStringLiteral("token"), QString::fromLatin1(token));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

}

Client::Response Anikoto::request(Client *client, const QString &url,
                                const QMap<QString, QString> &headers,
                                bool expectJson, bool endAt404) const {
    Client scoped = client->withSession(QStringLiteral("anikoto"));
    const QUrl resolved = QUrl(hostUrl()).resolved(QUrl(url));
    const auto response = scoped.get(resolved.toString(), headers);
    if (client->isCancelled() || (endAt404 && response.code == 404)) return {};
    if (response.code < 200 || response.code >= 300 || !response.error.isEmpty())
        throw AppException(tr("%1%2 returned %3: %4")
                               .arg(resolved.host(), resolved.path()).arg(response.code)
                               .arg(response.error.isEmpty() ? tr("provider request failed") : response.error), name());
    if (expectJson && response.toJson().isNull())
        throw AppException(tr("AniKoto returned an invalid API response for %1").arg(resolved.path()), name());
    return response;
}

QList<ShowData> Anikoto::parseShowList(const QString &html, bool resultsOnly) {
    QList<ShowData> shows;
    auto doc = Html::parse(html);
    if (!doc) return shows;
    auto posters = doc.select(QStringLiteral("%1//div[contains(@class,'poster') and @data-tip]")
                                  .arg(resultsOnly ? QStringLiteral("//div[@id='list-items']") : QString()));
    for (const auto &p : std::as_const(posters)) {
        QString id = p.attr("data-tip");
        if (id.isEmpty()) continue;
        auto img = p.selectFirst(".//img");
        QString cover = img ? img.attr("src") : QString();
        QString title = img ? img.attr("alt") : QString();
        auto name = p.selectFirst("..//a[contains(@class,'d-title')]");
        if (name) {
            QString t = name.text().simplified();
            if (!t.isEmpty()) title = t;
        }
        if (title.isEmpty()) continue;
        shows.emplaceBack(title, id, cover, this, "", ShowData::Anime);
    }
    return shows;
}

QList<ShowData> Anikoto::search(Client *client, const QString &query, int page, int /*typeIndex*/) {
    if (query.trimmed().isEmpty()) return {};
    QString url = hostUrl() + "filter?keyword=" + QUrl::toPercentEncoding(query)
                + "&page=" + QString::number(page);
    // Past the last page the filter answers 404.
    return parseShowList(request(client, url, m_headers, false, page > 1).body, true);
}

QVariantMap Anikoto::filterOptions(Client *client, int /*typeIndex*/) {
    // The filter page's own checkboxes, each followed by its label.
    const auto doc = Html::parse(request(client, hostUrl() + "filter", m_headers).body);
    if (!doc) return {};
    QVariantMap options;
    for (const QString field : {QStringLiteral("genre"), QStringLiteral("year")}) {
        QVariantList choices;
        for (const auto &input : doc.select(QStringLiteral("//input[@name='%1[]']").arg(field))) {
            const auto label = input.selectFirst(QStringLiteral("following-sibling::label[1]"));
            const QString value = input.attr(QStringLiteral("value"));
            choices.append(filterOption(value, label ? label.text().simplified() : value));
        }
        options.insert(field, choices);
    }
    options.insert(QStringLiteral("status"), statusOptions({"airing", "finished", "upcoming"}));
    return options;
}

QList<ShowData> Anikoto::filtered(Client *client, const QString &query, int page, int /*typeIndex*/,
                                  const QVariantMap &filters, bool latest) {
    static const QHash<QString, QString> kStatus = {
        {"airing", "currently-airing"}, {"finished", "finished-airing"}, {"upcoming", "not-yet-aired"}};
    // Words sort by relevance; without them the button picks the order.
    const QString sort = !query.trimmed().isEmpty() ? QStringLiteral("default")
                       : latest ? QStringLiteral("latest-updated") : QStringLiteral("most-viewed");
    QString url = hostUrl() + "filter?keyword=" + QUrl::toPercentEncoding(query) + "&sort=" + sort
                + "&page=" + QString::number(page);
    const auto add = [&url](const char *field, const QString &value) {
        if (!value.isEmpty()) url += QStringLiteral("&%1%5B%5D=").arg(QLatin1String(field)) + QUrl::toPercentEncoding(value);
    };
    add("genre", filters.value("genre").toString());
    add("year", filters.value("year").toString());
    add("status", kStatus.value(filters.value("status").toString()));
    return parseShowList(request(client, url, m_headers, false, page > 1).body, true);
}

QList<ShowData> Anikoto::popular(Client *client, int page, int /*typeIndex*/) {
    QString url = hostUrl() + "ajax/home/widget/trending?page=" + QString::number(page);
    return parseShowList(request(client, url, m_headers, true).toJsonObject().value("result").toString());
}

QList<ShowData> Anikoto::latest(Client *client, int page, int /*typeIndex*/) {
    QString url = hostUrl() + "ajax/home/widget/updated-sub?page=" + QString::number(page);
    return parseShowList(request(client, url, m_headers, true).toJsonObject().value("result").toString());
}

int Anikoto::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    const QString epUrl = hostUrl() + "ajax/episode/list/" + show.link + "?vrf=";
    const QString epHtml = request(client, epUrl, m_headers, true).toJsonObject().value("result").toString();
    const auto doc = Html::parse(epHtml);
    const auto eps = doc ? doc.select("//a[@data-ids and @data-num]") : QVector<Html::Node>{};

    if (parts.testFlag(Episodes)) {
        for (const auto &ep : std::as_const(eps)) {
            const QString ids = ep.attr("data-ids");
            if (ids.isEmpty()) continue;
            // `data-ids` is the episode's server key.
            show.addEpisode(0, ep.attr("data-num").toFloat(), ids, ep.attr("data-title").simplified());
        }
    }

    // No worker may hold a reference after an episode request throws.
    if (parts.testFlag(Details) && !client->isCancelled()) loadDetails(client, show);
    return eps.size();
}

// .bmeta rows read "<label>: <span>value</span>".
static QString metaField(const Html &page, const char *label) {
    auto node = page.selectFirst(QStringLiteral("//div[contains(@class,'bmeta')]//div[contains(text(),'%1')]/span")
                                     .arg(QLatin1String(label)));
    return node ? node.text().simplified() : QString();
}

void Anikoto::loadDetails(Client *client, ShowData &show) const {
    // The tooltip synopsis is truncated server-side.
    const auto tip = Html::parse(request(client, hostUrl() + "ajax/anime/tooltip/" + show.link, m_headers).body);
    const auto watchLink = tip ? tip.selectFirst("//div[contains(@class,'actions')]//a[contains(@class,'watch')]")
                               : Html::Node{};
    const QString watchUrl = watchLink ? watchLink.attr("href") : QString();
    if (watchUrl.isEmpty()) return;

    const auto page = Html::parse(request(client, watchUrl, m_headers).body);
    if (!page) return;

    if (auto synopsis = page.selectFirst("//div[contains(@class,'synopsis')]//div[contains(@class,'content')]"))
        show.description = synopsis.text().simplified();

    show.status      = metaField(page, "Status");
    // An unknown end date renders as "Nov 30, 1899".
    show.releaseDate = metaField(page, "Aired").remove(QRegularExpression(QStringLiteral(" to [^,]+, 1899$")));
    show.score       = metaField(page, "MAL");

    const auto genreLinks = page.select("//div[contains(@class,'bmeta')]//div[contains(text(),'Genres')]/span/a");
    for (const auto &genre : genreLinks) {
        const QString name = genre.text().simplified();
        if (!name.isEmpty()) show.genres.push_back(name);
    }

    // The banner is GMT; the countdown carries the same instant.
    if (auto countdown = page.selectFirst("//div[contains(@class,'next-episode')]//span[@data-target]")) {
        if (const qint64 epoch = countdown.attr("data-target").toLongLong(); epoch > 0)
            show.nextEpisodeAt = QDateTime::fromSecsSinceEpoch(epoch);
    }
}

QList<VideoServer> Anikoto::loadServers(Client *client, const PlaylistItem *episode) const {
    QList<VideoServer> servers;
    QString url = hostUrl() + "ajax/server/list?servers=" + QUrl::toPercentEncoding(episode->link);
    QString html = request(client, url, m_headers, true).toJsonObject().value("result").toString();
    auto doc = Html::parse(html);
    if (!doc) return servers;

    auto typeDivs = doc.select("//div[contains(@class,'type') and @data-type]");
    for (const auto &td : std::as_const(typeDivs)) {
        const QString type = td.attr("data-type");
        const bool isDub = type == QStringLiteral("dub");
        const QString suffix = isDub ? QStringLiteral(" Dub")
                             : type == QStringLiteral("hsub") ? QStringLiteral(" HSub")
                                                               : QStringLiteral(" Sub");
        auto tr = isDub ? VideoServer::Dub : VideoServer::Sub;
        auto lis = td.select(".//li[@data-link-id]");
        for (const auto &li : std::as_const(lis)) {
            QString linkId = li.attr("data-link-id");
            if (linkId.isEmpty()) continue;
            servers.emplaceBack(li.text().simplified() + suffix, linkId, tr);
        }
    }
    return servers;
}

PlayInfo Anikoto::extractSource(Client *client, VideoServer server) {
    // data-link-id -> the embed page url
    QString getUrl = hostUrl() + "ajax/server?get=" + QUrl::toPercentEncoding(server.link);
    QJsonObject result = request(client, getUrl, m_headers, true).toJsonObject().value("result").toObject();
    QString embedUrl = result.value("url").toString();
    if (embedUrl.isEmpty()) return {};
    return extractEmbed(client, embedUrl, server);
}

PlayInfo Anikoto::extractEmbed(Client *client, const QString &embedUrl, const VideoServer &server) const {
    PlayInfo info;
    QUrl u(embedUrl);
    QString origin = u.scheme() + "://" + u.host();

    QMap<QString, QString> pageHeaders = m_headers;
    pageHeaders["Referer"] = hostUrl();
    QString page = request(client, embedUrl, pageHeaders).body;

    // megaplay/megacloud family: the player id is on #megaplay.
    static const QRegularExpression idRe(R"RX(id="megaplay-player"[^>]*?data-id="(\d+)")RX");
    auto m = idRe.match(page);
    QString id = m.hasMatch() ? m.captured(1) : QString();
    if (id.isEmpty()) {
        static const QRegularExpression anyId(R"RX(data-id="(\d+)")RX");
        auto m2 = anyId.match(page);
        if (m2.hasMatch()) id = m2.captured(1);
    }
    if (id.isEmpty()) return info;

    QMap<QString, QString> srcHeaders = m_headers;
    srcHeaders["Referer"] = embedUrl;
    // Newer mirrors encrypt the source in `enc`; AniKoto rotates them.
    QStringList failures;
    Client scoped = client->withSession(QStringLiteral("anikoto"));
    for (const QString &endpoint : {QStringLiteral("stream/getSourcesNew"), QStringLiteral("stream/getSources")}) {
        const QString srcUrl = origin + "/" + endpoint + "?id=" + id;
        const auto response = scoped.get(srcUrl, srcHeaders);
        if (client->isCancelled()) return {};
        if (response.code < 200 || response.code >= 300 || !response.error.isEmpty()) {
            failures << QStringLiteral("%1 returned %2%3")
                            .arg(endpoint).arg(response.code)
                            .arg(response.error.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(response.error));
            continue;
        }

        const QJsonObject json = response.toJsonObject();
        if (json.isEmpty()) {
            failures << endpoint + QStringLiteral(" returned invalid JSON");
            continue;
        }

        QJsonObject decrypted;
        const QString encrypted = json.value(QStringLiteral("enc")).toString();
        if (!encrypted.isEmpty()) {
            decrypted = decryptMegaPlaySource(encrypted);
            if (decrypted.isEmpty()) {
                failures << endpoint + QStringLiteral(" returned an unreadable encrypted source");
                continue;
            }
        }

        QString file = sourceUrl(decrypted);
        if (file.isEmpty()) file = sourceUrl(json.value(QStringLiteral("sources")));
        if (file.isEmpty()) file = sourceUrl(json.value(QStringLiteral("source")));
        if (file.isEmpty()) {
            failures << endpoint + QStringLiteral(" did not include a playable source");
            continue;
        }
        const QString rawFile = file;
        const QString streamReferer = origin + "/";

        QJsonArray tracks = json.value(QStringLiteral("tracks")).toArray();
        if (tracks.isEmpty()) tracks = decrypted.value(QStringLiteral("tracks")).toArray();
        for (const QJsonValue &t : std::as_const(tracks)) {
            QJsonObject to = t.toObject();
            if (to.value("kind").toString() != "captions") continue;
            QString subUrl = to.value("file").toString();
            if (!subUrl.isEmpty())
                info.subtitles.emplaceBack(QUrl(subUrl), to.value("label").toString());
        }
        // The subtitle host 403s without the embed origin as Referer.
        info.addHeader("Referer", streamReferer);
        info.addHeader("User-Agent", m_headers["User-Agent"]);

        info.refreshPrimaryVideoUrl = [rawFile, streamReferer] {
            const QString freshFile = addMegaPlayCdnToken(rawFile);
            HlsProxy *proxy = HlsProxy::instance();
            return QUrl(proxy ? proxy->playlistUrl(freshFile, streamReferer) : freshFile);
        };
        info.videos.emplaceBack(info.refreshPrimaryVideoUrl(), server.name);
        return info;
    }

    throw AppException(tr("MegaPlay could not resolve player %1: %2")
                           .arg(id, failures.isEmpty() ? tr("no source endpoint responded")
                                                      : failures.join(QStringLiteral("; "))),
                       name());
}
