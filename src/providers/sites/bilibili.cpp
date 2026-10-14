#include "providers/sites/bilibili.h"
#include "app/mainwindow.h"
#include "app/async.h"
#include "media/player/danmaku.h"
#include <QtConcurrent/QtConcurrentRun>
#include "app/exception.h"
#include "app/logger.h"
#include "app/settings.h"
#include "system/platform.h"
#include "providers/showdata.h"
#include "net/client.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QHostInfo>
#include <QLocale>
#include <QMutex>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QPointer>
#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QGuiApplication>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <optional>
#include <qrcodegen.hpp>

namespace {
QPointer<Bilibili> g_instance;
}

Bilibili::Bilibili(QObject *parent) : Provider(parent), m_account(new BilibiliAccount(this)) {
    g_instance = this;
    reloadCredentials();
    connect(m_account, &Account::sessionChanged, this, [this]() { reloadCredentials(); });

    connect(&Settings::instance(), &Settings::settingsChanged, this, [this]() {
        if (reloadCredentials()) logInfo() << "Sources" << QStringLiteral("Bilibili:") << "credentials refreshed";
    });
}

Account *Bilibili::account() { return m_account; }

bool Bilibili::reloadCredentials() {
    QMutexLocker lock(&m_credentialsMutex);
    m_proxyApi = Settings::instance().get(Config::BilibiliProxy);
    m_cdnHost = Settings::instance().get(Config::BilibiliCdnHost).trimmed();

    const QString previousCookie = m_headers.value(QStringLiteral("Cookie"));

    m_headers = {
                 {"Referer",    "https://www.bilibili.com/"},
                 {"Origin",     "https://www.bilibili.com"},
                 {"User-Agent", kFirefoxUserAgent},
                 {"Accept",     "application/json, text/plain, */*"},
                 };

    const auto cookieMap = m_account->fields();
    if (!cookieMap.isEmpty()) {
        // An expired bili_ticket is worse than none: risk control reads it as a replayed session (-352).
        const qint64 ticketExpires =
            cookieMap.value(QStringLiteral("bili_ticket_expires")).toLongLong();
        const bool ticketLapsed = ticketExpires > 0
                                  && ticketExpires <= QDateTime::currentSecsSinceEpoch();

        QStringList parts;
        parts.reserve(cookieMap.size());
        for (auto it = cookieMap.constBegin(); it != cookieMap.constEnd(); ++it) {
            if (ticketLapsed && (it.key() == QLatin1String("bili_ticket")
                                 || it.key() == QLatin1String("bili_ticket_expires")))
                continue;
            QString value = it.value();
            value.replace(QLatin1Char(','), QLatin1String("%2C"));
            value.replace(QLatin1Char('*'), QLatin1String("%2A"));
            parts << (it.key() + QLatin1Char('=') + value);
        }
        static std::atomic_bool told = false;
        if (ticketLapsed && !told.exchange(true))
            logInfo() << "Sources" << QStringLiteral("Bilibili:") << "the stored bili_ticket expired on"
                      << QDateTime::fromSecsSinceEpoch(ticketExpires).toString(Qt::ISODate)
                      << "- renewing and saving it";
        m_headers["Cookie"] = parts.join("; ");
        m_csrf = cookieMap.value(QStringLiteral("bili_jct"));
    } else {
        m_csrf.clear();
    }

    const QString cookie = m_headers.value(QStringLiteral("Cookie"));
    const bool first = !m_credentialsLoaded;
    m_credentialsLoaded = true;
    if (cookie == previousCookie && !first) return false;

    if (previousCookie.isEmpty()) {
        if (cookieMap.isEmpty())
            logWarn() << "Sources" << QStringLiteral("Bilibili:") << "no cookies configured - member content will be unavailable";
        else
            logInfo() << "Sources" << QStringLiteral("Bilibili:") << "cookies loaded:" << cookieMap.size() << "entries";
    }
    m_syncRefused.store(false, std::memory_order_relaxed);
    return !previousCookie.isEmpty();
}

bool Bilibili::isSignedIn() const {
    QMutexLocker lock(&m_credentialsMutex);
    return !m_csrf.isEmpty();
}

// Provider calls run on workers while a sign-in rewrites these.
QString Bilibili::proxyApi() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_proxyApi;
}

QString Bilibili::cdnHost() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_cdnHost;
}

QMap<QString, QString> Bilibili::headers() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_headers;
}

QString Bilibili::csrf() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_csrf;
}

// Risk control refuses a bare client (unsigned view answers 412, a lapsed bili_ticket silences danmaku), so a browser-shaped session is kept.

namespace {

constexpr char kTicketHmacKey[] = "XgwSnGZ1p";

constexpr int kMixinTable[] = {
    46, 47, 18,  2, 53,  8, 23, 32, 15, 50, 10, 31, 58,  3, 45, 35,
    27, 43,  5, 49, 33,  9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48,  7, 16, 24, 55, 40, 61, 26, 17,  0,  1, 60, 51, 30,  4,
    22, 25, 54, 21, 56, 59,  6, 63, 57, 62, 11, 36, 20, 34, 44, 52,
};

constexpr qint64 kWbiKeyTtlMs = 6LL * 60 * 60 * 1000;
constexpr qint64 kRetryAfterMs = 60000;

struct WebSession {
    QMutex  mutex;
    QString buvid3, buvid4, bNut;
    QString ticket;
    qint64  ticketExpiresAt = 0;
    QString imgKey, subKey;
    qint64  keysAt = 0;
    qint64  lastAttemptMs = 0;
};

WebSession &webSession() {
    static WebSession state;
    return state;
}

QString wbiKeyOf(const QString &url) {
    return url.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

QMap<QString, QString> bootstrapHeaders() {
    return {
        {QStringLiteral("User-Agent"), QString::fromLatin1(kFirefoxUserAgent)},
        {QStringLiteral("Referer"),    QStringLiteral("https://www.bilibili.com/")},
        {QStringLiteral("Origin"),     QStringLiteral("https://www.bilibili.com")},
        {QStringLiteral("Accept"),     QStringLiteral("application/json, text/plain, */*")},
    };
}

Client bootstrapClient(Client *client) {
    Client scoped = client ? *client : Client({}, false);
    scoped.setVerbose(false).setBypassEnabled(false);
    return scoped.withSession(QStringLiteral("bilibili"));
}

void fetchFingerprint(Client *client, WebSession &state) {
    if (!state.buvid3.isEmpty()) return;
    Client scoped = bootstrapClient(client);
    const QJsonObject data =
        scoped.get(QStringLiteral("https://api.bilibili.com/x/frontend/finger/spi"),
                   bootstrapHeaders()).toJsonObject().value("data").toObject();
    state.buvid3 = data.value(QStringLiteral("b_3")).toString();
    state.buvid4 = data.value(QStringLiteral("b_4")).toString();
    if (!state.buvid3.isEmpty())
        state.bNut = QString::number(QDateTime::currentSecsSinceEpoch());
}

void keepTicketLater(const QString &ticket, qint64 expiresAt) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [ticket, expiresAt] {
        if (g_instance) static_cast<BilibiliAccount *>(g_instance->account())->keepTicket(ticket, expiresAt);
    }, Qt::QueuedConnection);
}

void fetchTicket(Client *client, WebSession &state, const QString &csrf) {
    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const bool ticketLive = !state.ticket.isEmpty() && state.ticketExpiresAt > nowSec + 300;
    const bool keysLive   = !state.imgKey.isEmpty()
                            && QDateTime::currentMSecsSinceEpoch() - state.keysAt < kWbiKeyTtlMs;
    if (ticketLive && keysLive) return;

    const QString signature = QString::fromLatin1(
        QMessageAuthenticationCode::hash(QByteArrayLiteral("ts") + QByteArray::number(nowSec),
                                         QByteArrayLiteral(kTicketHmacKey),
                                         QCryptographicHash::Sha256).toHex());
    Client scoped = bootstrapClient(client);
    const QString url = Client::urlWithParams(
        QStringLiteral("https://api.bilibili.com/bapis/bilibili.api.ticket.v1.Ticket/GenWebTicket"),
        {{QStringLiteral("key_id"),      QStringLiteral("ec02")},
         {QStringLiteral("hexsign"),     signature},
         {QStringLiteral("context[ts]"), QString::number(nowSec)},
         {QStringLiteral("csrf"),        csrf}});
    const QJsonObject data =
        scoped.post(url, QByteArray(), bootstrapHeaders()).toJsonObject().value("data").toObject();

    if (const QString issued = data.value(QStringLiteral("ticket")).toString(); !issued.isEmpty()) {
        state.ticket = issued;
        const qint64 ttl = data.value(QStringLiteral("ttl")).toInteger(259200);
        state.ticketExpiresAt = data.value(QStringLiteral("created_at")).toInteger(nowSec) + ttl;
        keepTicketLater(state.ticket, state.ticketExpiresAt);
    }
    const QJsonObject nav = data.value(QStringLiteral("nav")).toObject();
    const QString img = wbiKeyOf(nav.value(QStringLiteral("img")).toString());
    const QString sub = wbiKeyOf(nav.value(QStringLiteral("sub")).toString());
    if (img.size() == 32 && sub.size() == 32) {
        state.imgKey = img;
        state.subKey = sub;
        state.keysAt = QDateTime::currentMSecsSinceEpoch();
    }
}

void fetchWbiKeys(Client *client, WebSession &state) {
    if (!state.imgKey.isEmpty()
        && QDateTime::currentMSecsSinceEpoch() - state.keysAt < kWbiKeyTtlMs) return;
    Client scoped = bootstrapClient(client);
    const QJsonObject wbi =
        scoped.get(QStringLiteral("https://api.bilibili.com/x/web-interface/nav"), bootstrapHeaders())
            .toJsonObject().value("data").toObject().value(QStringLiteral("wbi_img")).toObject();
    const QString img = wbiKeyOf(wbi.value(QStringLiteral("img_url")).toString());
    const QString sub = wbiKeyOf(wbi.value(QStringLiteral("sub_url")).toString());
    if (img.size() != 32 || sub.size() != 32) return;
    state.imgKey = img;
    state.subKey = sub;
    state.keysAt = QDateTime::currentMSecsSinceEpoch();
}

QString mixinKey(const WebSession &state) {
    const QString raw = state.imgKey + state.subKey;
    if (raw.size() < 64) return {};
    QString mixed;
    mixed.reserve(32);
    for (int i = 0; i < 32; ++i) mixed.append(raw.at(kMixinTable[i]));
    return mixed;
}

QString sessionCookie(const WebSession &state) {
    QStringList parts;
    if (!state.buvid3.isEmpty()) parts << QStringLiteral("buvid3=") + state.buvid3;
    if (!state.buvid4.isEmpty()) parts << QStringLiteral("buvid4=") + state.buvid4;
    if (!state.bNut.isEmpty())   parts << QStringLiteral("b_nut=") + state.bNut;
    if (!state.ticket.isEmpty()) {
        parts << QStringLiteral("bili_ticket=") + state.ticket;
        parts << QStringLiteral("bili_ticket_expires=") + QString::number(state.ticketExpiresAt);
    }
    return parts.join(QStringLiteral("; "));
}

QString mergeCookies(const QString &existing, const QString &extra) {
    if (extra.isEmpty()) return existing;
    if (existing.isEmpty()) return extra;
    QSet<QString> have;
    for (const QString &pair : existing.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        have.insert(pair.section(QLatin1Char('='), 0, 0).trimmed());
    QStringList merged{existing};
    for (const QString &pair : extra.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (!have.contains(pair.section(QLatin1Char('='), 0, 0).trimmed()))
            merged << pair.trimmed();
    return merged.join(QStringLiteral("; "));
}

}

void Bilibili::primeWebSession(Client *client) {
    WebSession &state = webSession();
    QMutexLocker lock(&state.mutex);

    const qint64 nowMs  = QDateTime::currentMSecsSinceEpoch();
    const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
    const bool complete = !state.buvid3.isEmpty() && !state.imgKey.isEmpty()
                          && !state.ticket.isEmpty() && state.ticketExpiresAt > nowSec + 300
                          && nowMs - state.keysAt < kWbiKeyTtlMs;
    if (complete || nowMs - state.lastAttemptMs < kRetryAfterMs) return;
    state.lastAttemptMs = nowMs;

    fetchFingerprint(client, state);
    fetchTicket(client, state, QString());
    fetchWbiKeys(client, state);

    if (state.buvid3.isEmpty() || state.imgKey.isEmpty())
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "could not mint a browser session; risk control may refuse "
                                   "listings and danmaku";
}

QMap<QString, QString> Bilibili::withWebSession(Client *client, QMap<QString, QString> headers) {
    primeWebSession(client);
    WebSession &state = webSession();
    QMutexLocker lock(&state.mutex);
    if (const QString merged = mergeCookies(headers.value(QStringLiteral("Cookie")),
                                            sessionCookie(state));
        !merged.isEmpty())
        headers[QStringLiteral("Cookie")] = merged;
    return headers;
}

QMap<QString, QString> Bilibili::anonymousHeaders(Client *client) {
    primeWebSession(client);
    WebSession &state = webSession();
    QMap<QString, QString> headers = bootstrapHeaders();
    QMutexLocker lock(&state.mutex);
    if (const QString cookie = sessionCookie(state); !cookie.isEmpty())
        headers[QStringLiteral("Cookie")] = cookie;
    return headers;
}

// QUrl's query encoding leaves `+` and `,` alone and breaks the signature; this is encodeURIComponent, as bilibili signs.
static QString encodeComponent(const QString &text) {
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

QString Bilibili::signedQuery(Client *client, QMap<QString, QString> params) {
    primeWebSession(client);

    QString mixin;
    {
        WebSession &state = webSession();
        QMutexLocker lock(&state.mutex);
        mixin = mixinKey(state);
    }
    static const QRegularExpression unencodable(QStringLiteral("[!'()*]"));
    if (!mixin.isEmpty()) {
        params[QStringLiteral("wts")] = QString::number(QDateTime::currentSecsSinceEpoch());
        for (auto it = params.begin(); it != params.end(); ++it) it.value().remove(unencodable);

        // QMap iterates in key order, which is the order the signature is defined over.
        QStringList signedPairs;
        signedPairs.reserve(params.size());
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            signedPairs << encodeComponent(it.key()) + QLatin1Char('=') + encodeComponent(it.value());
        params[QStringLiteral("w_rid")] = QString::fromLatin1(
            QCryptographicHash::hash((signedPairs.join(QLatin1Char('&')) + mixin).toUtf8(),
                                     QCryptographicHash::Md5).toHex());
    }

    QStringList pairs;
    pairs.reserve(params.size());
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        pairs << encodeComponent(it.key()) + QLatin1Char('=') + encodeComponent(it.value());
    return pairs.join(QLatin1Char('&'));
}

Client::Response Bilibili::apiGet(Client *client, const QString &url,
                                  const QMap<QString, QString> &params) const {
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    auto requestHeaders = withWebSession(client, headers());
    const QString relay = proxyApi();
    if (relay.isEmpty())
        return scoped.get(url, requestHeaders, params);

    requestHeaders["X-Proxy-Url"] = Client::urlWithParams(url, params);
    return scoped.get(relay, requestHeaders);
}

Client::Response Bilibili::apiGetSigned(Client *client, const QString &url,
                                        QMap<QString, QString> params) const {
    const QString query = signedQuery(client, std::move(params));
    return apiGet(client, query.isEmpty() ? url : url + QLatin1Char('?') + query);
}

QList<ShowData> Bilibili::videos(const QJsonArray &items) const {
    QList<ShowData> shows;
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        const QString bvid = o["bvid"].toString();
        if (!bvid.startsWith(QLatin1String("BV")) || (o.contains("goto") && o["goto"].toString() != "av")) continue;
        QString title = o["title"].toString();
        title.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
        QString cover = o["pic"].toString();
        if (cover.startsWith(QLatin1String("//"))) cover.prepend(QLatin1String("https:"));
        else if (cover.startsWith(QLatin1String("http://"))) cover.insert(4, QLatin1Char('s'));
        if (!cover.isEmpty() && !cover.contains(QLatin1Char('@'))) cover += QStringLiteral("@480w_270h_1c.jpg");
        int secs = o["duration"].toInt();
        if (secs <= 0)
            for (const QString &part : o["duration"].toString().split(QLatin1Char(':'), Qt::SkipEmptyParts))
                secs = secs * 60 + part.toInt();
        const QString length = secs <= 0 ? QString()
            : secs >= 3600 ? QStringLiteral("%1:%2:%3").arg(secs / 3600).arg(secs / 60 % 60, 2, 10, QLatin1Char('0'))
                                                        .arg(secs % 60, 2, 10, QLatin1Char('0'))
                           : QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
        const QString owner = o.contains("owner") ? o["owner"].toObject()["name"].toString() : o["author"].toString();
        shows.emplaceBack(title, bvid, cover, const_cast<Bilibili *>(this), length, ShowData::Video);
        shows.last().coverAspect = 9.0 / 16;
        shows.last().subtitle = owner;
    }
    return shows;
}

QList<ShowData> Bilibili::search(Client *client, const QString &query, int page, int typeIndex) {
    QString searchType = typeIndex == kVideoType ? "video" : (typeIndex <= 1) ? "media_bangumi" : "media_ft";
    QMap<QString, QString> params = {
                                     {"search_type", searchType},
                                     {"keyword",     query},
                                     {"page",        QString::number(page)},
                                     {"page_size",   "20"},
                                     {"platform",    "pc"},
                                     };

    const QJsonObject data =
        apiGetSigned(client, "https://api.bilibili.com/x/web-interface/wbi/search/type", params)
            .toJsonObject()["data"].toObject();
    // Risk control answers code 0 with a voucher in place of results.
    if (data.contains(QStringLiteral("v_voucher")))
        throw AppException(tr("Bilibili's risk control blocked this search. Signing in under Settings > Accounts & "
                              "sync usually helps."), name());
    const QJsonArray results = data["result"].toArray();
    if (typeIndex == kVideoType) return videos(results);

    QList<ShowData> shows;
    for (const auto &v : std::as_const(results)) {
        auto r = v.toObject();
        QString title = r["title"].toString();
        title.remove("<em class=\"keyword\">").remove("</em>");
        QString link = QStringLiteral("%1 %2")
                           .arg(QString::number(r["media_id"].toInt()),
                                QString::number(r["season_id"].toInt()));
        shows.emplaceBack(title, link, r["cover"].toString(), this,
                          r["index_show"].toString(), kShowTypes[typeIndex]);
    }
    return shows;
}

QList<ShowData> Bilibili::popular(Client *client, int page, int typeIndex) {
    if (typeIndex == kVideoType)
        return videos(apiGet(client, "https://api.bilibili.com/x/web-interface/popular",
                             {{"pn", QString::number(page)}, {"ps", "20"}})
                          .toJsonObject()["data"].toObject()["list"].toArray());
    return filterSearch(client, (typeIndex <= 1) ? 3 : 2, page, typeIndex);
}

QList<ShowData> Bilibili::latest(Client *client, int page, int typeIndex) {
    if (typeIndex == kVideoType) {
        const QString n = QString::number(page);
        return videos(apiGetSigned(client, "https://api.bilibili.com/x/web-interface/wbi/index/top/feed/rcmd",
                                   {{"web_location", "1430650"}, {"y_num", "4"}, {"fresh_type", "4"},
                                    {"feed_version", "V8"}, {"fresh_idx", n}, {"fresh_idx_1h", n},
                                    {"brush", n}, {"fetch_row", "1"}, {"homepage_ver", "1"}, {"ps", "20"}})
                          .toJsonObject()["data"].toObject()["item"].toArray());
    }
    return filterSearch(client, 0, page, typeIndex);
}

QVariantMap Bilibili::filterOptions(Client *client, int typeIndex) {
    if (typeIndex == kVideoType) return {};
    const QJsonObject data = apiGet(client, "https://api.bilibili.com/pgc/season/index/condition",
                                    {{"season_type", QString::number(kSeasonTypes[typeIndex])}, {"type", "1"}})
                                 .toJsonObject()["data"].toObject();
    static const QRegularExpression span(QStringLiteral(R"(^\[(\d{4})?[^,]*,(\d{4})?)"));
    const auto yearLabel = [](const QString &value, const QString &name) {
        const auto match = span.match(value);
        const int from = match.captured(1).toInt(), to = match.captured(2).toInt();
        if (from && to) return to - from == 1 ? QString::number(from) : QStringLiteral("%1–%2").arg(from).arg(to - 1);
        return to ? QStringLiteral("< %1").arg(to) : name;
    };
    QVariantMap options;
    for (const QJsonValue &entry : data["filter"].toArray()) {
        const QString field = entry["field"].toString();
        const QString key = field == "style_id" ? "genre"
                          : field == "year" || field == "release_date" ? "year"
                          : field == "is_finish" ? "status" : QString();
        if (key.isEmpty()) continue;
        QVariantList choices;
        for (const QJsonValue &choice : entry["values"].toArray()) {
            const QString value = choice["keyword"].toString();
            if (value == "-1") continue;
            if (key == "status")
                choices.append(statusOptions({value == "1" ? "finished" : "airing"}));
            else
                choices.append(filterOption(value, key == "year" ? yearLabel(value, choice["name"].toString())
                                                                 : choice["name"].toString()));
        }
        options.insert(key, choices);
    }
    return options;
}

QList<ShowData> Bilibili::filtered(Client *client, const QString & , int page, int typeIndex,
                                   const QVariantMap &filters, bool latest) {
    if (typeIndex == kVideoType) return latest ? this->latest(client, page, typeIndex) : popular(client, page, typeIndex);
    return filterSearch(client, latest ? 0 : (typeIndex <= 1) ? 3 : 2, page, typeIndex, filters);
}

QList<ShowData> Bilibili::filterSearch(Client *client, int sortBy, int page, int typeIndex,
                                       const QVariantMap &filters) {
    int st = kSeasonTypes[typeIndex];
    QMap<QString, QString> params = {
                                     {"st",             QString::number(st)},
                                     {"season_type",    QString::number(st)},
                                     {"order",          QString::number(sortBy)},
                                     {"sort",           "0"},
                                     {"page",           QString::number(page)},
                                     {"pagesize",       "20"},
                                     {"type",           "1"},
                                     {"style_id",       "-1"},
                                     {"season_version", "-1"},
                                     {"is_finish",      "-1"},
                                     {"copyright",      "-1"},
                                     {"season_status",  "-1"},
                                     {"year",           "-1"},
                                     };
    if (const QString genre = filters.value("genre").toString(); !genre.isEmpty())
        params["style_id"] = genre;
    if (const QString year = filters.value("year").toString(); !year.isEmpty())
        params[st == 1 || st == 4 ? "year" : "release_date"] = year;
    if (const QString status = filters.value("status").toString(); !status.isEmpty())
        params["is_finish"] = status == "finished" ? "1" : "0";

    // Encoded here: QUrlQuery leaves a year span's brackets and spaces as they are; the index answers those with a 400.
    QStringList pairs;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        pairs << encodeComponent(it.key()) + QLatin1Char('=') + encodeComponent(it.value());
    auto list = apiGet(client, "https://api.bilibili.com/pgc/season/index/result?" + pairs.join(QLatin1Char('&')), {})
                    .toJsonObject()["data"].toObject()["list"].toArray();

    QList<ShowData> shows;
    for (const auto &v : std::as_const(list)) {
        auto s = v.toObject();
        QString link = QStringLiteral("%1 %2")
                           .arg(QString::number(s["media_id"].toInt()),
                                QString::number(s["season_id"].toInt()));
        shows.emplaceBack(s["title"].toString(), link, s["cover"].toString(),
                          this, s["index_show"].toString(), kShowTypes[typeIndex]);
    }
    return shows;
}

bool Bilibili::parseUrl(const QUrl &url, QString &showLink, int &episodeIndex) const {
    const QString host = url.host().toLower();
    if (host != QLatin1String("bilibili.com") && !host.endsWith(QLatin1String(".bilibili.com")))
        return false;

    static const QRegularExpression videoRe(QStringLiteral(R"(^/video/(BV[0-9A-Za-z]{10})/?$)"));
    static const QRegularExpression bangumiRe(QStringLiteral(R"(^/bangumi/play/(ep|ss)(\d+)/?$)"));

    if (const auto video = videoRe.match(url.path()); video.hasMatch()) {
        showLink = video.captured(1);
        const int page = QUrlQuery(url).queryItemValue(QStringLiteral("p")).toInt();
        episodeIndex = page > 0 ? page - 1 : -1;
        return true;
    }
    if (const auto bangumi = bangumiRe.match(url.path()); bangumi.hasMatch()) {
        showLink = bangumi.captured(1) + ' ' + bangumi.captured(2);
        episodeIndex = -1;
        return true;
    }
    return false;
}

int Bilibili::loadShow(Client *client, ShowData &show, LoadParts parts) const {
    return show.link.startsWith(QLatin1String("BV")) ? loadVideo(client, show, parts)
                                                     : loadSeason(client, show, parts);
}

QString Bilibili::showUrl(const QString &link) const {
    if (link.startsWith(QLatin1String("BV"))) return hostUrl() + "video/" + link;
    const QStringList ids = link.split(' ');
    if (ids.size() < 2) return {};
    return hostUrl() + "bangumi/play/" + (ids[0] == QLatin1String("ep") ? "ep" : "ss") + ids[1];
}

// A higher tier gets each episode a day early (its pub_time); everyone else at the next regular slot. 0 without such a schedule.
qint64 Bilibili::regularSlotAfter(const QString &schedule, qint64 earlyAt) {
    if (earlyAt <= 0 || !schedule.contains(QStringLiteral("抢先"))) return 0;
    static const QRegularExpression slot(QStringLiteral("每周([一二三四五六日天])\\s*(\\d{1,2})[:：](\\d{2})更新"));
    const auto match = slot.match(schedule);
    if (!match.hasMatch()) return 0;
    const QString day = match.captured(1);
    const int weekday = day == QStringLiteral("天") ? 7 : int(QStringLiteral("一二三四五六日").indexOf(day)) + 1;
    const QTimeZone china = QTimeZone::fromSecondsAheadOfUtc(8 * 3600);
    const QDate from = QDateTime::fromSecsSinceEpoch(earlyAt, china).date();
    const QTime at(match.captured(2).toInt(), match.captured(3).toInt());
    for (int ahead = 0; ahead <= 7; ++ahead) {
        const QDateTime candidate(from.addDays(ahead), at, china);
        if (candidate.date().dayOfWeek() == weekday && candidate.toSecsSinceEpoch() > earlyAt)
            return candidate.toSecsSinceEpoch();
    }
    return 0;
}

int Bilibili::loadSeason(Client *client, ShowData &show, LoadParts parts) const {
    const QStringList ids = show.link.split(' ');
    if (ids.size() < 2) return 0;
    const bool fromUrl = ids[0] == QLatin1String("ep") || ids[0] == QLatin1String("ss");
    const QString startEpId = ids[0] == QLatin1String("ep") ? ids[1] : QString();

    auto json = apiGet(client, "https://api.bilibili.com/pgc/view/web/season",
                       {{ids[0] == QLatin1String("ep") ? "ep_id" : "season_id", ids[1]}}).toJsonObject();

    auto result = json["result"].toObject();
    if (result.isEmpty()) {
        if (!client->isCancelled())
            logWarn() << "Sources" << QStringLiteral("Bilibili:") << "Failed to load season" << show.link
                      << json["code"].toInt() << json["message"].toString();
        return 0;
    }

    const QString seasonId = QString::number(result["season_id"].toInteger());
    if (fromUrl) {
        show.link = QString::number(result["media_id"].toInteger()) + ' ' + seasonId;
        if (show.title.isEmpty()) show.title = result["title"].toString();
    }

    auto episodeList = result["episodes"].toArray();

    int episodeCount = 0;
    for (const auto &v : std::as_const(episodeList)) {
        if (v.toObject()["badge"].toString() != QStringLiteral("预告"))
            episodeCount++;
    }

    if (parts.testFlag(CountOnly)) return episodeCount;

    if (parts.testFlag(Episodes)) {
        const QString schedule = result["new_ep"].toObject()["desc"].toString();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        int startIndex = -1;
        for (int i = 0; i < episodeList.size(); ++i) {
            auto ep = episodeList[i].toObject();
            bool isPreview = (ep["badge"].toString() == QStringLiteral("预告"));
            if (isPreview && i != episodeList.size() - 1) continue;

            QString epId = QString::number(ep["ep_id"].toInteger());
            QString link = seasonId + '&' + epId;
            if (const qint64 epCid = ep["cid"].toInteger(); epCid > 0) {
                link += '&' + QString::number(epCid);
                if (const qint64 aid = ep["aid"].toInteger(); aid > 0) link += '&' + QString::number(aid);
            }
            QString title = ep["title"].toString();
            QString longTitle = ep["long_title"].toString();

            bool ok;
            float number = title.toFloat(&ok);
            const QString name = isPreview ? ep["show_title"].toString(QStringLiteral("(预告) ") + longTitle)
                               : !ok && !longTitle.isEmpty() ? title + QLatin1Char(' ') + longTitle
                               : ok ? longTitle : title;
            show.addEpisode(0, ok && !isPreview ? number : -1, link, name, isPreview);
            if (const auto added = show.playlist()->last()) {
                added->thumbnail = ep["cover"].toString().replace(QLatin1String("http://"), QLatin1String("https://"));
                added->airedAt = ep["pub_time"].toInteger();
                if (const qint64 opens = isPreview ? 0 : regularSlotAfter(schedule, added->airedAt); opens > now)
                    added->availableAt = opens;
            }
            if (epId == startEpId) startIndex = show.playlist()->count() - 1;
        }
        if (startIndex >= 0) show.playlist()->setCurrentIndex(startIndex);
    }

    if (!parts.testFlag(Details)) return episodeCount;

    show.coverUrl    = result["cover"].toString();
    show.description = result["evaluate"].toString();
    show.releaseDate = result["publish"].toObject()["pub_time_show"].toString();
    show.updateTime  = result["new_ep"].toObject()["desc"].toString();

    auto stat = result["stat"].toObject();
    show.views  = QLocale::system().toString(stat["views"].toInteger());
    show.status = stat["follow_text"].toString();

    auto rating = result["rating"].toObject();
    if (!rating.isEmpty()) {
        show.score = QStringLiteral("%1 (%2)")
        .arg(QString::number(rating["score"].toDouble()),
             QLocale::system().toString(rating["count"].toInt()));
    }

    const auto pushLabel = [&show](const QJsonValue &value) {
        const QString label = value.isObject() ? value.toObject()["name"].toString()
                                               : value.toString();
        if (!label.isEmpty() && !show.genres.contains(label)) show.genres.push_back(label);
    };
    for (const QJsonValue &style : result["styles"].toArray()) pushLabel(style);
    for (const QJsonValue &area : result["areas"].toArray()) pushLabel(area);

    return episodeCount;
}

int Bilibili::loadVideo(Client *client, ShowData &show, LoadParts parts) const {
    const QJsonObject data = apiGetSigned(client, "https://api.bilibili.com/x/web-interface/wbi/view",
                                          {{"bvid", show.link}}).toJsonObject()["data"].toObject();
    const QJsonArray pages = data["pages"].toArray();
    if (pages.isEmpty()) {
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "Failed to load video" << show.link;
        return 0;
    }
    if (parts.testFlag(CountOnly)) return pages.size();

    if (show.title.isEmpty()) show.title = data["title"].toString();

    if (parts.testFlag(Episodes)) {
        for (const QJsonValue &v : pages) {
            const QJsonObject page = v.toObject();
            show.addEpisode(0, float(page["page"].toInt()),
                            show.link + '&' + QString::number(page["cid"].toInteger()),
                            pages.size() > 1 ? page["part"].toString() : QString());
        }
        if (pages.size() == 1 && show.playlist()) show.playlist()->solo = true;
    }

    if (parts.testFlag(Details)) {
        show.coverUrl    = data["pic"].toString();
        show.coverAspect = 9.0 / 16;
        show.description = data["desc"].toString();
        show.releaseDate = QDateTime::fromSecsSinceEpoch(data["pubdate"].toInteger()).toString("yyyy-MM-dd");
        show.updateTime  = data["owner"].toObject()["name"].toString();
        show.views       = QLocale::system().toString(data["stat"].toObject()["view"].toInteger());
        if (const QString category = data["tname"].toString(); !category.isEmpty())
            show.genres.push_back(category);
    }
    return pages.size();
}

QList<VideoServer> Bilibili::loadServers(Client * , const PlaylistItem *episode) const {
    return {{"Default", episode->link}};
}

bool Bilibili::reportProgress(Client *client, const QString &episodeLink, double seconds, double duration) {
    const QString token = csrf();
    if (token.isEmpty() || m_syncRefused.load(std::memory_order_relaxed)) return false;
    const QStringList parts = episodeLink.split('&');
    QMap<QString, QString> form{
        {"csrf", token}, {"dt", "2"}, {"play_type", "0"},
        {"played_time", QString::number(qint64(seconds))},
        {"video_duration", QString::number(qint64(duration))},
    };
    if (parts[0].startsWith(QLatin1String("BV"))) {
        if (parts.size() < 2) return false;
        form["bvid"] = parts[0];
        form["cid"]  = parts[1];
        form["type"] = "3";
    } else {
        if (parts.size() < 4) return false;
        form["sid"]  = parts[0];
        form["epid"] = parts[1];
        form["cid"]  = parts[2];
        form["aid"]  = parts[3];
        form["type"] = "4";
        form["sub_type"] = "1";
    }
    const auto requestHeaders = withWebSession(client, headers());
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    scoped.setVerbose(false);
    const auto response = scoped.post("https://api.bilibili.com/x/click-interface/web/heartbeat", form, requestHeaders);
    if (client->isCancelled()) return false;
    noteServerClock(response.header(QStringLiteral("Date")));
    const int code = response.toJsonObject().value("code").toInt(-1);
    if (code == 0) return true;
    const bool refused = code == -101 || code == -111;
    m_syncRefused.store(refused, std::memory_order_relaxed);
    logWarn() << "Sync" << QStringLiteral("Bilibili:") << "progress sync refused:" << (code == -1 ? response.error : QString::number(code))
              << (refused ? "- sign in again from Settings > Accounts" : "");
    return false;
}

void Bilibili::noteServerClock(const QString &dateHeader) {
    if (dateHeader.isEmpty()) return;
    const QDateTime served = QDateTime::fromString(dateHeader, Qt::RFC2822Date);
    if (!served.isValid()) return;
    const qint64 sample = served.toSecsSinceEpoch() - QDateTime::currentSecsSinceEpoch();
    const qint64 previous = m_clockSkew.load(std::memory_order_relaxed);
    m_clockSkew.store(previous == 0 ? sample : (previous * 3 + sample) / 4,
                      std::memory_order_relaxed);
}

WatchHistory::RemoteProgress Bilibili::fetchProgress(Client *client, const QString &episodeLink) {
    if (csrf().isEmpty() || m_syncRefused.load(std::memory_order_relaxed)) return {};
    const QStringList parts = episodeLink.split('&');

    QMap<QString, QString> params;
    if (parts[0].startsWith(QLatin1String("BV"))) {
        if (parts.size() < 2) return {};
        params = {{"bvid", parts[0]}, {"cid", parts[1]}};
    } else {
        if (parts.size() < 4) return {};
        params = {{"ep_id", parts[1]}, {"cid", parts[2]}, {"aid", parts[3]}};
    }

    const QString query = signedQuery(client, std::move(params));
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    scoped.setVerbose(false);
    const auto response =
        scoped.get(QStringLiteral("https://api.bilibili.com/x/player/wbi/v2?") + query,
                   withWebSession(client, headers()));
    if (client->isCancelled()) return {};
    noteServerClock(response.header(QStringLiteral("Date")));

    const QJsonObject json = response.toJsonObject();
    if (json.value("code").toInt(-1) != 0) {
        logWarn() << "Sync" << QStringLiteral("Bilibili:") << "progress read refused:" << json.value("code").toInt(-1);
        return {};
    }
    const QJsonObject data = json.value("data").toObject();
    if (!data.contains("last_play_time")) return {};

    RemoteProgress out;
    out.seconds = data.value("last_play_time").toDouble() / 1000.0;
    out.serverAt = QDateTime::currentSecsSinceEpoch() + clockSkew();
    out.valid = out.seconds > 0.0;
    return out;
}

namespace {

constexpr qint64 kDnsCacheMs = 60'000;
constexpr int kSurveyDeadlineMs = 3000;
constexpr int kMaxLookups = 20;

// Bounds shared with hostReplaced, which must cut at the same places.
qsizetype hostStart(const QString &url) {
    const qsizetype scheme = url.indexOf(QLatin1String("://"));
    return scheme < 0 ? -1 : scheme + 3;
}

QString hostOf(const QString &url) {
    const qsizetype from = hostStart(url);
    if (from < 0) return {};
    const qsizetype end = url.indexOf(QLatin1Char('/'), from);
    return url.mid(from, (end < 0 ? url.size() : end) - from);
}

// What DNS is asked: PCDN urls carry a port, and "host:8082" never resolves.
QString hostName(const QString &url) {
    QString host = hostOf(url);
    if (const qsizetype colon = host.lastIndexOf(QLatin1Char(':')); colon > 0) host.truncate(colon);
    return host;
}

// Lower is better. The site's pick for this region first; PCDN last: peer nodes cut files off midway and drop out of DNS.
int hostRank(const QString &host) {
    if (host.endsWith(QLatin1String(".akamaized.net"))) return 0;
    if (host.startsWith(QLatin1String("upos-")) && host.contains(QLatin1String("mirror")))
        return host.section(QLatin1Char('.'), 0, 0).endsWith(QLatin1String("ov")) ? 0 : 2;
    if (host.startsWith(QLatin1String("upos-"))) return 3;
    if (host.contains(QLatin1String("bcache")) || host.startsWith(QLatin1String("cn-"))) return 4;
    return 5;
}

QString hostReplaced(const QString &url, const QString &host) {
    const qsizetype from = hostStart(url);
    const qsizetype end = from < 0 ? -1 : url.indexOf(QLatin1Char('/'), from);
    if (end < 0) return url;
    return url.left(from) + host + url.mid(end);
}

QStringList streamMirrors(const QJsonObject &stream) {
    QStringList urls;
    for (const auto &key : {"baseUrl", "base_url", "url"}) {
        if (const QString u = stream[key].toString(); !u.isEmpty()) { urls << u; break; }
    }
    for (const auto &key : {"backupUrl", "backup_url"}) {
        const QJsonArray backups = stream[key].toArray();
        if (backups.isEmpty()) continue;
        for (const QJsonValue &v : backups)
            if (const QString u = v.toString(); !u.isEmpty()) urls << u;
        break;
    }

    QStringList perHost;
    QSet<QString> seen;
    for (const QString &url : std::as_const(urls)) {
        const QString host = hostName(url);
        if (!host.isEmpty() && !seen.contains(host)) {
            seen.insert(host);
            perHost << url;
        }
    }
    return perHost;
}

struct Stream {
    QJsonObject json;
    QStringList mirrors;
};

QList<Stream> streamsOf(const QJsonArray &group) {
    QList<Stream> streams;
    streams.reserve(group.size());
    for (const QJsonValue &v : group) {
        Stream stream{v.toObject(), {}};
        stream.mirrors = streamMirrors(stream.json);
        streams.append(std::move(stream));
    }
    return streams;
}

QStringList distinctHosts(std::initializer_list<const QList<Stream> *> groups) {
    QStringList hosts;
    QSet<QString> seen;
    for (const QList<Stream> *group : groups)
        for (const Stream &stream : *group)
            for (const QString &url : stream.mirrors)
                if (QString host = hostName(url); !host.isEmpty() && !seen.contains(host)) {
                    seen.insert(host);
                    hosts.append(std::move(host));
                }
    return hosts;
}

// playurl picks mirrors for the asker's IP: through the mainland relay many fail from abroad, plus PCDN nodes.
// A signed path moves between Tencent's mirrors, not onto Akamai (its urls carry their own token).
// From the UK: cosov 2-8 MB/s, akamai 1.5-10, cos/coso1 ~1.5; ali, hw, aliov, hwov, 08c failed.
class MirrorPicker {
public:
    explicit MirrorPicker(const QString &own) {
        if (!own.isEmpty()) m_targets << own;
        for (const char *host : {"upos-sz-mirrorcosov.bilivideo.com", "upos-sz-mirrorcos.bilivideo.com"})
            if (!m_targets.contains(QLatin1String(host))) m_targets << QString::fromLatin1(host);
        m_own = !own.isEmpty();
    }
    const QStringList &targets() const { return m_targets; }

    void survey(const Client *client, const QStringList &hosts) {
        QStringList unknown;
        for (const QString &host : hosts) {
            if (const auto cached = cachedAnswer(host)) classify(host, *cached);
            else if (unknown.size() < kMaxLookups) unknown << host;
        }
        if (unknown.isEmpty() || client->isCancelled()) return;

        QObject context;
        QEventLoop loop;
        QHash<QString, bool> answers;
        int pending = unknown.size();
        for (const QString &host : std::as_const(unknown)) {
            QHostInfo::lookupHost(host, &context, [&, host](const QHostInfo &info) {
                answers.insert(host, info.error() == QHostInfo::NoError);
                if (--pending == 0) loop.quit();
            });
        }
        QTimer::singleShot(kSurveyDeadlineMs, &loop, &QEventLoop::quit);
        loop.exec();

        for (const QString &host : std::as_const(unknown)) {
            const auto it = answers.constFind(host);
            if (it == answers.constEnd()) { m_dead.insert(host); continue; }
            remember(host, *it);
            classify(host, *it);
        }
    }

    QStringList ranked(const QStringList &mirrors) {
        struct Option { int score; QString url; };
        QList<Option> options;
        QSet<QString> taken;
        const auto add = [&](int score, const QString &url) {
            const QString host = hostName(url);
            if (host.isEmpty() || taken.contains(host) || !m_reachable.contains(host)) return;
            taken.insert(host);
            options.append({score, url});
        };
        for (const QString &url : mirrors) add(hostRank(hostName(url)) * 10, url);
        for (const QString &url : mirrors) {
            const QString path = url.mid(hostStart(url) + hostOf(url).size());
            if (hostName(url).endsWith(QLatin1String(".akamaized.net")) || path.startsWith(QLatin1String("/v1/resource")))
                continue;
            for (int i = 0; i < m_targets.size(); ++i)
                add(m_own && i == 0 ? -1 : hostRank(m_targets[i]) * 10 + 5 + i, hostReplaced(url, m_targets[i]));
            break;
        }
        std::stable_sort(options.begin(), options.end(),
                         [](const Option &a, const Option &b) { return a.score < b.score; });
        QStringList urls;
        for (const Option &option : std::as_const(options)) {
            urls << option.url;
            if (urls.size() == 4) break;
        }
        if (urls.isEmpty() && !mirrors.isEmpty()) urls << mirrors.first();
        if (!urls.isEmpty() && !mirrors.contains(urls.first())) ++m_retargeted;
        return urls;
    }

    int reachable() const  { return m_reachable.size(); }
    int dead() const       { return m_dead.size(); }
    int retargeted() const { return m_retargeted; }

private:
    void classify(const QString &host, bool resolved) {
        if (resolved) m_reachable << host;
        else          m_dead.insert(host);
    }

    // QHash::operator[] default-constructs, so an unresolved host must read as unreachable.
    struct Answer { bool resolved = false; qint64 atMs = 0; };
    static QMutex &cacheMutex() { static QMutex mutex; return mutex; }
    static QHash<QString, Answer> &cache() { static QHash<QString, Answer> cache; return cache; }

    static std::optional<bool> cachedAnswer(const QString &host) {
        QMutexLocker locked(&cacheMutex());
        const auto it = cache().constFind(host);
        if (it == cache().constEnd() || QDateTime::currentMSecsSinceEpoch() - it->atMs >= kDnsCacheMs)
            return std::nullopt;
        return it->resolved;
    }

    static void remember(const QString &host, bool resolved) {
        QMutexLocker locked(&cacheMutex());
        cache().insert(host, {resolved, QDateTime::currentMSecsSinceEpoch()});
    }

    QStringList   m_targets;
    bool          m_own = false;
    QStringList   m_reachable;
    QSet<QString> m_dead;
    int           m_retargeted = 0;
};

QList<QUrl> urlsOf(const QStringList &urls) {
    QList<QUrl> out;
    for (const QString &url : urls) out << QUrl(url, QUrl::StrictMode);
    return out;
}

}

static qint64 findInt(const QJsonValue &value, QLatin1String key, int depth = 0) {
    if (depth > 6) return 0;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        const auto hit = object.constFind(key);
        if (hit != object.constEnd() && hit->toInteger() > 0)
            return hit->toInteger();
        for (const QJsonValue &child : object)
            if (const qint64 found = findInt(child, key, depth + 1)) return found;
    } else if (value.isArray()) {
        for (const QJsonValue &child : value.toArray())
            if (const qint64 found = findInt(child, key, depth + 1)) return found;
    }
    return 0;
}

static QJsonObject unwrapSource(const QJsonObject &json) {
    QJsonObject root = json;
    if (root.contains("raw"))  root = root["raw"].toObject();
    if (root.contains("data")) root = root["data"].toObject();
    if (root.contains("result")) {
        auto inner = root["result"].toObject();
        if (inner.contains("video_info")) root = inner;
    }
    if (root.contains("video_info")) return root["video_info"].toObject();
    if (root.contains("dash") || root.contains("durl") || root.contains("durls")) return root;
    return {};
}

static QString audioQualityName(int id, const QString &fallback) {
    switch (id) {
    case 30216: return QStringLiteral("64K");
    case 30232: return QStringLiteral("132K");
    case 30280: return QStringLiteral("192K");
    case 30250: return QStringLiteral("杜比全景声");
    case 30251: return QStringLiteral("Hi-Res无损");
    }
    return fallback;
}

static QString videoQualityName(int qn, int height) {
    switch (qn) {
    case 6:   return QStringLiteral("240P 极速");
    case 16:  return QStringLiteral("360P 流畅");
    case 32:  return QStringLiteral("480P 清晰");
    case 64:  return QStringLiteral("720P 高清");
    case 74:  return QStringLiteral("720P60 高帧率");
    case 80:  return QStringLiteral("1080P 高清");
    case 100: return QStringLiteral("智能修复");
    case 112: return QStringLiteral("1080P 高码率");
    case 116: return QStringLiteral("1080P60 高帧率");
    case 120: return QStringLiteral("4K 超清");
    case 125: return QStringLiteral("HDR 真彩色");
    case 126: return QStringLiteral("杜比视界");
    case 127: return QStringLiteral("8K 超高清");
    case 129: return QStringLiteral("HDR Vivid");
    }
    return height > 0 ? QStringLiteral("%1P").arg(height) : QStringLiteral("Q%1").arg(qn);
}

static QString videoCodecName(int codecid) {
    switch (codecid) {
    case 7:  return QStringLiteral("AVC");
    case 12: return QStringLiteral("HEVC");
    case 13: return QStringLiteral("AV1");
    }
    return {};
}

static int videoRank(int qn, int codecid) {
    const bool special = qn == 125 || qn == 126 || qn == 129;
    const int codec = codecid == 13 ? 2 : codecid == 12 ? 1 : 0;
    return (special ? 0 : qn) * 10 + codec;
}

Source Bilibili::extractSource(Client *client, VideoServer server) {
    Source source;

    const QStringList parts = server.link.split('&');
    if (parts.size() < 2) return source;
    const bool upload = parts[0].startsWith(QLatin1String("BV"));
    const qint64 listedCid = upload ? parts[1].toLongLong()
                           : parts.size() > 2 ? parts[2].toLongLong() : 0;
    const qint64 listedAid = !upload && parts.size() > 3 ? parts[3].toLongLong() : 0;

    QMap<QString, QString> params = {{"fnval", "4048"}, {"fnver", "0"}, {"fourk", "1"}};
    if (upload) {
        params["bvid"] = parts[0];
        params["cid"]  = parts[1];
    } else {
        params["ep_id"] = parts[1];
    }

    auto json = apiGet(client, upload ? "https://api.bilibili.com/x/player/playurl"
                                      : "https://api.bilibili.com/pgc/player/web/v2/playurl", params)
                    .toJsonObject();

    auto videoInfo = unwrapSource(json);

    const int code = json["code"].toInt(-1);
    const bool isPreviewing = videoInfo["is_preview"].toInt(0) == 1;
    const int quality = videoInfo["quality"].toInt();
    logInfo() << "Sources" << QStringLiteral("Bilibili:") << "playurl: code=" << code
           << "preview=" << isPreviewing
           << "quality=" << quality
           << "dash=" << videoInfo.contains("dash")
           << "videos=" << videoInfo["dash"].toObject()["video"].toArray().size();

    // A trial is a 200 carrying the first few minutes, and `timelength` is then the trial's.
    if (isPreviewing) {
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "preview/trial content returned; the account cannot watch this in full";
        throw AppException(
            isSignedIn()
                ? tr("Bilibili only offers a preview of this episode to your account. "
                     "It is member-only, region-locked, or not yet released.")
                : tr("Bilibili only offers a preview of this episode when signed out. Sign in under Settings > "
                     "Accounts & sync to watch it in full."),
            QStringLiteral("Bilibili"));
    }
    if (code != 0 && videoInfo.isEmpty()) {
        const QString message = json["message"].toString();
        QString hint;
        switch (code) {
        case -101: case -403:
            hint = tr("Sign in under Settings > Accounts & sync, or use an account with access."); break;
        case -10403: case 6002003:
            hint = tr("Bilibili offers it only in mainland China. Set bilibili/proxy to a mainland relay."); break;
        case -404: case 6002001: case 6002004:
            hint = tr("It may not be out yet, or Bilibili took it down."); break;
        case -352: case -412:
            hint = tr("Bilibili's risk control blocked the request. Try again in a minute."); break;
        }
        const QString what = message.isEmpty() ? QString::number(code) : QStringLiteral("%1: %2").arg(code).arg(message);
        throw AppException(tr("Bilibili refused this episode (%1).").arg(what)
                               + (hint.isEmpty() ? QString() : QStringLiteral("\n\n") + hint),
                           QStringLiteral("Bilibili"));
    }

    if (const qint64 cid = listedCid > 0 ? listedCid : findInt(json, QLatin1String("cid")); cid > 0) {
        const int durationMs = int(findInt(json, QLatin1String("timelength")));
        const qint64 aid = listedAid > 0 ? listedAid : findInt(json, QLatin1String("aid"));
        offerDanmaku(source, QStringLiteral("bili-%1").arg(cid),
                     {{"cid", cid}, {"aid", aid}, {"durationMs", durationMs}});
    }

    MirrorPicker mirrors(cdnHost());

    QHash<int, QString> qualityNames;
    for (const QJsonValue &v : videoInfo["support_formats"].toArray()) {
        const QJsonObject format = v.toObject();
        if (const QString name = format["new_description"].toString().trimmed(); !name.isEmpty())
            qualityNames.insert(format["quality"].toInt(), name);
    }
    const auto qualityName = [&qualityNames](int qn, int height) {
        return qualityNames.value(qn, videoQualityName(qn, height));
    };
    if (videoInfo.contains("dash")) {
        const QJsonObject dash = videoInfo["dash"].toObject();
        QJsonArray hiRes;
        if (const QJsonValue lossless = dash["flac"].toObject()["audio"]; lossless.isObject())
            hiRes.append(lossless);
        const QList<Stream> flac   = streamsOf(hiRes);
        const QList<Stream> dolby  = streamsOf(dash["dolby"].toObject()["audio"].toArray());
        const QList<Stream> audios = streamsOf(dash["audio"].toArray());
        const QList<Stream> videos = streamsOf(dash["video"].toArray());

        mirrors.survey(client, distinctHosts({&videos, &audios, &dolby, &flac}) + mirrors.targets());

        auto addAudio = [&](const Stream &stream, const QString &fallbackName) {
            const int bw = stream.json["bandwidth"].toInt();
            if (const QStringList urls = mirrors.ranked(stream.mirrors); !urls.isEmpty()) {
                source.audios.emplaceBack(QUrl(urls.first(), QUrl::StrictMode),
                                            audioQualityName(stream.json["id"].toInt(), fallbackName), "", bw);
                source.audios.last().alternates = urlsOf(urls.mid(1));
            }
        };
        for (const Stream &stream : flac)   addAudio(stream, QStringLiteral("Hi-Res无损"));
        for (const Stream &stream : dolby)  addAudio(stream, QStringLiteral("杜比全景声"));
        for (const Stream &stream : audios) addAudio(stream, Track::formatBitrate(stream.json["bandwidth"].toInt()));

        for (const Stream &stream : videos) {
            const int h  = stream.json["height"].toInt();
            const int bw = stream.json["bandwidth"].toInt();
            QString label = qualityName(stream.json["id"].toInt(), h);
            if (const QString codec = videoCodecName(stream.json["codecid"].toInt()); !codec.isEmpty())
                label += QStringLiteral(" · ") + codec;
            if (const QStringList urls = mirrors.ranked(stream.mirrors); !urls.isEmpty()) {
                source.videos.emplaceBack(QUrl(urls.first(), QUrl::StrictMode), label, h, bw);
                source.videos.last().alternates = urlsOf(urls.mid(1));
                source.videos.last().rank = videoRank(stream.json["id"].toInt(), stream.json["codecid"].toInt());
            }
        }
    }
    else if (videoInfo.contains("durl")) {
        const QList<Stream> durl = streamsOf(videoInfo["durl"].toArray());
        mirrors.survey(client, distinctHosts({&durl}) + mirrors.targets());
        for (const Stream &stream : durl) {
            if (const QStringList urls = mirrors.ranked(stream.mirrors); !urls.isEmpty()) {
                source.videos.emplaceBack(QUrl(urls.first(), QUrl::StrictMode),
                                            qualityName(videoInfo["quality"].toInt(), 0));
                source.videos.last().alternates = urlsOf(urls.mid(1));
            }
        }
    } else if (videoInfo.contains("durls")) {
        QJsonArray inner;
        QList<int> qualities;
        for (const QJsonValue &v : videoInfo["durls"].toArray()) {
            const QJsonObject item = v.toObject();
            const QJsonObject first = item["durl"].toArray().at(0).toObject();
            if (first.isEmpty()) continue;
            inner.append(first);
            qualities.append(item["quality"].toInt());
        }
        const QList<Stream> durls = streamsOf(inner);
        mirrors.survey(client, distinctHosts({&durls}) + mirrors.targets());
        for (qsizetype i = 0; i < durls.size(); ++i) {
            if (const QStringList urls = mirrors.ranked(durls[i].mirrors); !urls.isEmpty()) {
                source.videos.emplaceBack(QUrl(urls.first(), QUrl::StrictMode), qualityName(qualities[i], 0));
                source.videos.last().alternates = urlsOf(urls.mid(1));
            }
        }
    } else {
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "No video streams found in response";
        throw AppException(tr("Bilibili has no video for this episode yet. Try again later."),
                           QStringLiteral("Bilibili"));
    }

    logInfo() << "Sources" << QStringLiteral("Bilibili:") << "Extracted:" << source.videos.size() << "video,"
           << source.audios.size() << "audio streams;" << "mirrors"
           << mirrors.reachable() << "reachable," << mirrors.dead() << "unresolvable";
    if (!source.videos.isEmpty())
        logInfo() << "Sources" << QStringLiteral("Bilibili:") << "Playing from" << source.videos.first().url.host()
                  << "with" << source.videos.first().alternates.size() << "fallback host(s)";
    if (mirrors.retargeted() > 0)
        logInfo() << "Sources" << QStringLiteral("Bilibili:") << mirrors.retargeted() << "stream(s) moved onto a better mirror";
    if (mirrors.reachable() == 0)
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "no playurl mirror resolves - check DNS or the bilibili/proxy relay";

    source.addHeader("Referer", "https://www.bilibili.com/");
    source.addHeader("User-Agent", headers().value("User-Agent"));
    return source;
}

namespace {

constexpr char kGenerate[] = "https://passport.bilibili.com/x/passport-login/web/qrcode/generate";
constexpr char kPoll[]     = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll";

QMap<QString, QString> loginHeaders() {
    return {
        {QStringLiteral("Referer"),    QStringLiteral("https://passport.bilibili.com/login")},
        {QStringLiteral("Origin"),     QStringLiteral("https://passport.bilibili.com")},
        {QStringLiteral("User-Agent"), QString::fromLatin1(kFirefoxUserAgent)},
        {QStringLiteral("Accept"),     QStringLiteral("application/json, text/plain, */*")},
    };
}

const QStringList &requiredCookies() {
    static const QStringList names{QStringLiteral("SESSDATA"), QStringLiteral("bili_jct"),
                                   QStringLiteral("DedeUserID")};
    return names;
}

}

BilibiliAccount::LoginTicket BilibiliAccount::beginLogin(Client *client) {
    if (!client) return {};
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    const auto response = scoped.setBypassEnabled(false).get(QLatin1String(kGenerate), loginHeaders());

    const QJsonObject json = response.toJsonObject();
    if (response.code != 200 || json.value(QStringLiteral("code")).toInt(-1) != 0) {
        logWarn() << "Sources" << QStringLiteral("Bilibili:") << "could not start a sign-in:" << response.code
                  << json.value(QStringLiteral("message")).toString();
        return {};
    }

    const QJsonObject data = json.value(QStringLiteral("data")).toObject();
    LoginTicket ticket;
    ticket.qrcodeKey  = data.value(QStringLiteral("qrcode_key")).toString();
    ticket.confirmUrl = QUrl(data.value(QStringLiteral("url")).toString());
    if (!ticket.isValid()) logWarn() << "Sources" << QStringLiteral("Bilibili:") << "the sign-in ticket came back incomplete";
    return ticket;
}

BilibiliAccount::LoginPoll BilibiliAccount::pollLogin(Client *client, const LoginTicket &ticket) {
    LoginPoll result;
    if (!client || !ticket.isValid()) { result.status = LoginStatus::Failed; return result; }

    Client scoped = client->withSession(QStringLiteral("bilibili"));
    const auto response = scoped.setBypassEnabled(false).get(
        QLatin1String(kPoll), loginHeaders(),
        {{QStringLiteral("qrcode_key"), ticket.qrcodeKey}});

    const QJsonObject json = response.toJsonObject();
    if (response.code != 200 || json.isEmpty()) { result.status = LoginStatus::Failed; return result; }

    const QJsonObject data = json.value(QStringLiteral("data")).toObject();
    switch (data.value(QStringLiteral("code")).toInt(-1)) {
    case 86101: result.status = LoginStatus::Pending; return result;
    case 86090: result.status = LoginStatus::Scanned; return result;
    case 86038: result.status = LoginStatus::Expired; return result;
    case 0:     break;
    default:    result.status = LoginStatus::Failed;  return result;
    }

    const QUrlQuery query(QUrl(data.value(QStringLiteral("url")).toString()));
    for (const auto &item : query.queryItems(QUrl::FullyDecoded))
        if (!item.second.isEmpty()) result.cookies.insert(item.first, item.second);

    for (const QString &name : requiredCookies()) {
        if (result.cookies.value(name).isEmpty()) {
            logWarn() << "Sources" << QStringLiteral("Bilibili:") << "the confirmation carried no" << name;
            result.status = LoginStatus::Failed;
            result.cookies.clear();
            return result;
        }
    }
    result.status = LoginStatus::Confirmed;
    return result;
}

namespace {
QString qrDataUrl(const QString &text) {
    const QByteArray data = text.toUtf8();
    if (data.isEmpty()) return {};
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(data.constData());
    std::optional<qrcodegen::QrCode> code;
    try {
        code = qrcodegen::QrCode::encodeBinary({bytes, bytes + data.size()}, qrcodegen::QrCode::Ecc::MEDIUM);
    } catch (const qrcodegen::data_too_long &) {
        return {};
    }
    constexpr int quietZone = 4;
    const int size = code->getSize();
    QImage image(size + quietZone * 2, size + quietZone * 2, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (code->getModule(x, y)) image.setPixel(x + quietZone, y + quietZone, qRgb(0, 0, 0));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}
}

BilibiliAccount::BilibiliAccount(QObject *parent) : Account(Config::BilibiliCookies, parent) {
    look();
    QTimer::singleShot(1500, this, [this]() { refresh(); });
}

BilibiliAccount::~BilibiliAccount() {
    m_loginCancel.cancel();
    m_checkCancel.cancel();
    waitFor(m_loginFuture, "Bilibili login");
    waitFor(m_accountFuture, "Bilibili account");
}

QString BilibiliAccount::label() const { return tr("Bilibili"); }

QString BilibiliAccount::hint() const {
    return tr("Sign in shows a QR code to scan with the Bilibili app. Browsing, search and free episodes work without "
              "an account.");
}

QString BilibiliAccount::offlineHint() const {
    return tr("Member content and sync need an account");
}

QStringList BilibiliAccount::required() const { return requiredCookies(); }

QDateTime BilibiliAccount::sessionEnd(const QString &sessdata) {
    const qint64 epoch = QUrl::fromPercentEncoding(sessdata.toUtf8()).section(QLatin1Char(','), 1, 1).toLongLong();
    const QDateTime end = QDateTime::fromSecsSinceEpoch(epoch);
    const QDateTime now = QDateTime::currentDateTime();
    return epoch > 0 && end > now.addYears(-2) && end < now.addYears(30) ? end : QDateTime();
}

// "vip_due_date" is milliseconds since the epoch; "vip_label.text" the site's own name for the plan.
Account::Standing BilibiliAccount::standingFrom(const QJsonObject &nav) {
    Standing standing;
    standing.signedIn = nav.value(QStringLiteral("isLogin")).toBool();
    standing.name = nav.value(QStringLiteral("uname")).toString();
    standing.asked = true;
    standing.member = nav.value(QStringLiteral("vipStatus")).toInt() == 1;
    if (standing.member) {
        standing.plan = nav.value(QStringLiteral("vip_label")).toObject().value(QStringLiteral("text")).toString();
        if (standing.plan.isEmpty())
            standing.plan = nav.value(QStringLiteral("vipType")).toInt() == 2 ? QStringLiteral("年度大会员") : QStringLiteral("大会员");
    }
    if (const qint64 due = nav.value(QStringLiteral("vip_due_date")).toInteger(); due > 0)
        standing.until = QDateTime::fromMSecsSinceEpoch(due).date();
    const auto add = [&standing](const QString &label, const QString &value) { if (!value.isEmpty()) standing.details.append({label, value}); };
    if (const qint64 mid = nav.value(QStringLiteral("mid")).toInteger(); mid > 0) add(tr("User ID"), QString::number(mid));
    const QJsonObject level = nav.value(QStringLiteral("level_info")).toObject();
    if (level.contains(QStringLiteral("current_level"))) {
        const int now = level.value(QStringLiteral("current_level")).toInt();
        const qint64 exp = level.value(QStringLiteral("current_exp")).toInteger(-1), next = level.value(QStringLiteral("next_exp")).toInteger(-1);
        add(tr("Level"), exp >= 0 && next > 0 ? tr("Lv %1 · %2 / %3 exp").arg(now).arg(exp).arg(next) : tr("Lv %1").arg(now));
    }
    if (nav.contains(QStringLiteral("money"))) add(tr("Coins"), QString::number(nav.value(QStringLiteral("money")).toDouble(), 'f', 1));
    const QJsonObject wallet = nav.value(QStringLiteral("wallet")).toObject();
    if (wallet.contains(QStringLiteral("bcoin_balance"))) add(tr("B-coins"), QString::number(wallet.value(QStringLiteral("bcoin_balance")).toDouble(), 'f', 1));
    return standing;
}

void BilibiliAccount::look() {
    const QString sessdata = fields().value(QStringLiteral("SESSDATA"));
    Standing standing;
    standing.signedIn = !sessdata.isEmpty();
    standing.name = standing.signedIn ? Settings::instance().get(Config::BilibiliAccount) : QString();
    standing.sessionEnds = sessionEnd(sessdata);
    setStanding(standing);
}

void BilibiliAccount::adopted() {
    look();
    refresh();
}

bool BilibiliAccount::keepTicket(const QString &ticket, qint64 expiresAt) {
    Fields cookies = fields();
    if (cookies.isEmpty() || ticket.isEmpty()) return false;
    const qint64 stored = cookies.value(QStringLiteral("bili_ticket_expires")).toLongLong();
    if (stored >= expiresAt || stored > QDateTime::currentSecsSinceEpoch() + 86400) return false;
    cookies.insert(QStringLiteral("bili_ticket"), ticket);
    cookies.insert(QStringLiteral("bili_ticket_expires"), QString::number(expiresAt));
    keep(cookies);
    return true;
}

void BilibiliAccount::refresh() {
    if (!signedIn()) return;
    if (m_checking) {
        m_recheck = true;
        return;
    }
    m_checking = true;
    const Fields cookies = fields();
    QStringList jar;
    for (auto it = cookies.constBegin(); it != cookies.constEnd(); ++it) {
        if (it.value().isEmpty()) continue;
        // As the provider sends them: a comma or an asterisk in a value (SESSDATA) goes out percent-encoded.
        QString value = it.value();
        value.replace(QLatin1Char(','), QLatin1String("%2C"));
        value.replace(QLatin1Char('*'), QLatin1String("%2A"));
        jar << it.key() + QLatin1Char('=') + value;
    }
    setBusy(true);

    m_accountFuture = QtConcurrent::run([this, cookies, cookie = jar.join(QStringLiteral("; ")),
                                         ends = sessionEnd(cookies.value(QStringLiteral("SESSDATA"))),
                                         cancel = m_checkCancel]() {
        Client client(cancel, false);
        client.setStrictTls(true);
        const auto response = client.setBypassEnabled(false).get(
            QStringLiteral("https://api.bilibili.com/x/web-interface/nav"),
            {{"Cookie", cookie}, {"Referer", "https://www.bilibili.com/"}});
        if (cancel.isCancelled()) return;
        const QJsonObject data = response.toJsonObject().value("data").toObject();
        const bool answered = response.code == 200 && !data.isEmpty();
        Standing standing = standingFrom(data);
        standing.sessionEnds = ends;
        standing.checkedAt = QDateTime::currentDateTime();
        QMetaObject::invokeMethod(this, [this, standing, answered, cookies]() {
            m_checking = false;
            setBusy(false);
            if (std::exchange(m_recheck, false) || fields() != cookies) {
                // Asked again meanwhile, or pasted, edited or signed out: this answer is about other cookies.
                QTimer::singleShot(0, this, [this]() { refresh(); });
                return;
            }
            if (!answered) {
                Standing unchanged = this->standing();
                unchanged.checkedAt = standing.checkedAt;
                unchanged.problem = tr("Bilibili did not respond, so the account could not be checked.");
                setStanding(unchanged);
                return;
            }
            if (!standing.signedIn) {
                if (this->standing().problem.isEmpty())
                    logWarn() << "Sources" << QStringLiteral("Bilibili:") << "the stored session is no longer valid";
                Standing unchanged = this->standing();
                unchanged.checkedAt = standing.checkedAt;
                unchanged.problem = tr("Bilibili no longer accepts this session.");
                setStanding(unchanged);
                return;
            }
            Settings::instance().set(Config::BilibiliAccount, standing.name);
            setStanding(standing);
        }, Qt::QueuedConnection);
    });
}

void BilibiliAccount::copyLoginLink() const {
    if (!m_loginUrl.isEmpty()) QGuiApplication::clipboard()->setText(m_loginUrl);
}

void BilibiliAccount::showLogin(const QString &url, const QString &status) {
    setLogin(status, url.isEmpty() ? QString() : url == m_loginUrl ? loginQr() : qrDataUrl(url));
    m_loginUrl = url;
}

void BilibiliAccount::signedInWith(const Fields &cookies) {
    keep(cookies);
    showLogin({}, {});
    look();
    refresh();
    MainWindow::instance().reportInfo(tr("Signed in to %1.").arg(label()), label());
}

void BilibiliAccount::signIn() {
    if (m_loginFuture.isRunning()) return;
    showLogin({}, tr("Starting…"));

    m_loginFuture = QtConcurrent::run([this, cancel = m_loginCancel]() {
        Client client(cancel, false);
        const LoginTicket ticket = beginLogin(&client);
        if (cancel.isCancelled()) return;
        if (!ticket.isValid()) {
            QMetaObject::invokeMethod(this, [this]() {
                showLogin({}, {});
                MainWindow::instance().reportError(
                    tr("Could not start the Bilibili sign-in. Check the connection and try again."),
                    "Bilibili");
            }, Qt::QueuedConnection);
            return;
        }

        const QString url = ticket.confirmUrl.toString();
        QMetaObject::invokeMethod(this, [this, url]() {
            showLogin(url, tr("Scan with the Bilibili app"));
            if (loginQr().isEmpty())
                MainWindow::instance().reportError(
                    tr("Could not create the QR code. Copy the link and open it on your phone."), "Bilibili");
        }, Qt::QueuedConnection);

        // The ticket lives three minutes; once a second is what the web client does.
        const QDeadlineTimer deadline(180000);
        LoginPoll result;
        while (!deadline.hasExpired() && !cancel.isCancelled()) {
            QThread::msleep(1000);
            if (cancel.isCancelled()) return;
            result = pollLogin(&client, ticket);
            if (result.status == LoginStatus::Confirmed || result.status == LoginStatus::Expired) break;
            if (result.status == LoginStatus::Scanned)
                QMetaObject::invokeMethod(this, [this, url]() {
                    showLogin(url, tr("Confirm on your phone"));
                }, Qt::QueuedConnection);
        }
        if (cancel.isCancelled()) return;

        const auto status = result.status;
        const auto cookies = result.cookies;
        QMetaObject::invokeMethod(this, [this, status, cookies]() {
            if (status == LoginStatus::Confirmed && !cookies.isEmpty()) {
                signedInWith(cookies);
                return;
            }
            showLogin({}, {});
            MainWindow::instance().reportError(
                status == LoginStatus::Expired
                    ? tr("The Bilibili QR code expired. Try again.")
                    : tr("The Bilibili sign-in was not completed."),
                "Bilibili");
        }, Qt::QueuedConnection);
    });
}

void BilibiliAccount::cancelLogin() {
    m_loginCancel.cancel();
    m_loginCancel = CancelToken{};
    showLogin({}, {});
}

void BilibiliAccount::signOut() {
    Account::signOut();
    Settings::instance().set(Config::BilibiliAccount, QString());
}

namespace {

constexpr int kSegmentMs   = 360000;
constexpr int kMaxSegments = 40;

using u8 = quint8;

bool readVarint(const u8 *&p, const u8 *end, quint64 &out) {
    quint64 value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (p >= end) return false;
        const u8 byte = *p++;
        value |= quint64(byte & 0x7F) << shift;
        if (!(byte & 0x80)) { out = value; return true; }
        shift += 7;
    }
    return false;
}

bool skipField(const u8 *&p, const u8 *end, int wire) {
    quint64 scratch = 0;
    switch (wire) {
    case 0: return readVarint(p, end, scratch);
    case 1: if (end - p < 8) return false; p += 8; return true;
    case 5: if (end - p < 4) return false; p += 4; return true;
    case 2: {
        if (!readVarint(p, end, scratch)) return false;
        if (scratch > quint64(end - p)) return false;
        p += scratch;
        return true;
    }
    default: return false;
    }
}

bool parseElem(const u8 *p, const u8 *end, DanmakuComment &out) {
    while (p < end) {
        quint64 key = 0;
        if (!readVarint(p, end, key)) return false;
        const int field = int(key >> 3);
        const int wire  = int(key & 7);
        quint64 v = 0;

        if (field == 2 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.timeMs = int(qMin<quint64>(v, INT_MAX));
        } else if (field == 3 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.mode = int(v);
        } else if (field == 4 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.fontSize = int(v);
        } else if (field == 5 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.color = quint32(v) & 0xFFFFFFu;
        } else if (field == 7 && wire == 2) {
            if (!readVarint(p, end, v)) return false;
            if (v > quint64(end - p)) return false;
            out.text = QString::fromUtf8(reinterpret_cast<const char *>(p), qsizetype(v));
            p += v;
        } else if (field == 9 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.weight = int(v);
            out.rated = true;
        } else if (field == 11 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.pool = int(v);
        } else if (field == 13 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.attr = int(v);
        } else if (field == 24 && wire == 0) {
            if (!readVarint(p, end, v)) return false;
            out.colorful = v == 60001;
        } else if (!skipField(p, end, wire)) {
            return false;
        }
    }
    return true;
}

QList<DanmakuComment> parseSegment(const QByteArray &data) {
    QList<DanmakuComment> out;
    const u8 *p   = reinterpret_cast<const u8 *>(data.constData());
    const u8 *end = p + data.size();

    while (p < end) {
        quint64 key = 0;
        if (!readVarint(p, end, key)) break;
        const int field = int(key >> 3);
        const int wire  = int(key & 7);

        if (field == 1 && wire == 2) {
            quint64 len = 0;
            if (!readVarint(p, end, len) || len > quint64(end - p)) break;
            DanmakuComment c;
            // Absent means unrated; 0 would let minWeight erase everything.
            c.weight = 10;
            if (parseElem(p, p + len, c) && !c.text.isEmpty())
                out.append(std::move(c));
            p += len;
        } else if (!skipField(p, end, wire)) {
            break;
        }
    }
    return out;
}

Client::Response fetchBytes(Client &worker, const QString &url, const QMap<QString, QString> &params,
                            const QMap<QString, QString> &headers, const QString &proxyApi) {
    if (proxyApi.isEmpty()) return worker.getBytes(url, headers, params);
    auto proxied = headers;
    proxied["X-Proxy-Url"] = Client::urlWithParams(url, params);
    return worker.getBytes(proxyApi, proxied);
}

QList<DanmakuComment> fetchLegacyXml(Client *client, qint64 cid,
                                     const QMap<QString, QString> &headers,
                                     const QString &proxyApi) {
    Client worker = *client;
    const auto response = fetchBytes(worker, QStringLiteral("https://api.bilibili.com/x/v1/dm/list.so"),
                                     {{"oid", QString::number(cid)}}, headers, proxyApi);
    if (response.code != 200 || response.bytes.isEmpty()) return {};

    QByteArray xml = Decompress::rawDeflate(response.bytes);
    if (xml.isEmpty()) xml = response.bytes;
    if (!xml.contains("<d ")) return {};

    return DanmakuAss::fromXml(xml);
}

using SegmentResult = Bilibili::DanmakuSegment;

// A refusal arrives as JSON on a 200, and decoding it as protobuf looks like no comments.
QString danmakuRefusal(const Client::Response &response) {
    const bool looksJson = response.header("Content-Type").contains(QLatin1String("json"))
                           || response.bytes.trimmed().startsWith('{');
    if (!looksJson) return {};
    const QJsonObject json = QJsonDocument::fromJson(response.bytes).object();
    const int code = json.value("code").toInt();
    if (code == 0) return {};
    const QString message = json.value("message").toString();
    return QStringLiteral("%1%2").arg(code)
        .arg(message.isEmpty() ? QString() : QStringLiteral(": ") + message);
}

SegmentResult fetchSegmentOnce(Client *client, qint64 cid, qint64 aid, int index,
                               const QMap<QString, QString> &headers, const QString &proxyApi) {
    QMap<QString, QString> params{{"type", "1"},
                                  {"oid", QString::number(cid)},
                                  {"segment_index", QString::number(index)}};
    if (aid > 0) params["pid"] = QString::number(aid);

    Client worker = *client;
    const auto response = fetchBytes(
        worker, QStringLiteral("https://api.bilibili.com/x/v2/dm/web/seg.so"), params,
        headers, proxyApi);

    SegmentResult result;
    if (worker.isCancelled()) return result;
    if (response.code <= 0) {
        result.refusal = response.error.isEmpty()
                             ? QStringLiteral("the request did not complete")
                             : response.error;
        return result;
    }
    // seg.so's -304 (Bili-Status-Code) means nothing in this segment, past the end or after six quiet
    // minutes: an answer, not a refusal.
    if (response.code == 304) return result;
    if (response.code != 200) {
        result.refusal = QStringLiteral("HTTP %1").arg(response.code);
        return result;
    }
    if (response.bytes.isEmpty()) return result;
    result.refusal = danmakuRefusal(response);
    if (result.refusal.isEmpty()) result.comments = parseSegment(response.bytes);
    return result;
}

SegmentResult fetchSegment(Client *client, qint64 cid, qint64 aid, int index,
                           const QMap<QString, QString> &headers, const QString &proxyApi) {
    SegmentResult result;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) QThread::msleep(500u << (attempt - 1));
        result = fetchSegmentOnce(client, cid, aid, index, headers, proxyApi);
        if (result.refusal.isEmpty() || client->isCancelled()) break;
    }
    return result;
}

}

namespace {

QList<DanmakuComment> collectSegments(Client *client, qint64 cid, qint64 aid, int durationMs,
                                      const QMap<QString, QString> &headers,
                                      const QString &proxyApi, QString &refusal) {
    QStringList missing;
    const QList<DanmakuComment> all = Bilibili::walkDanmaku(
        client, durationMs,
        [client, cid, aid, headers, proxyApi](int index) {
            return fetchSegment(client, cid, aid, index, headers, proxyApi);
        },
        refusal, missing);
    if (!missing.isEmpty() && !all.isEmpty())
        logWarn() << "Danmaku" << "cid" << cid << "has no comments for" << missing.join(QStringLiteral(", "))
                  << "- the site turned those parts away:" << refusal;
    return all;
}

}

QList<DanmakuComment> Bilibili::walkDanmaku(Client *client, int durationMs,
                                            const std::function<DanmakuSegment(int index)> &fetch,
                                            QString &refusal, QStringList &missing) {
    const int expected = qBound(1, durationMs > 0 ? (durationMs + kSegmentMs - 1) / kSegmentMs : 4,
                                kMaxSegments);

    QList<DanmakuComment> all;
    int index = 1;
    while (index <= kMaxSegments && !client->isCancelled()) {
        // Firing forty requests to find the end at six gets you rate-limited; six at a time for the length promised, then one by one.
        const int batch = index <= expected ? qMin(6, expected - index + 1) : 1;
        QList<QFuture<DanmakuSegment>> futures;
        futures.reserve(batch);
        for (int i = index; i < index + batch; ++i)
            futures << QtConcurrent::run([&fetch, i] { return fetch(i); });

        bool lastWasEmpty = false;
        for (int i = 0; i < futures.size(); ++i) {
            const DanmakuSegment part = futures[i].result();
            lastWasEmpty = part.comments.isEmpty();
            all.append(part.comments);
            if (!part.refusal.isEmpty() && index + i <= expected) {
                const int minute = (index + i - 1) * kSegmentMs / 60000;
                missing << QStringLiteral("%1-%2 min").arg(minute).arg(minute + kSegmentMs / 60000);
            }
            if (refusal.isEmpty()) refusal = part.refusal;
        }
        index += batch;
        if (lastWasEmpty && index > expected) break;
    }
    return all;
}

QList<DanmakuComment> Bilibili::fetchDanmaku(Client *client, qint64 cid, qint64 aid, int durationMs,
                                            const QMap<QString, QString> &headers,
                                            const QString &proxyApi) {
    if (!client || cid <= 0) return {};

    // Public, so an account can only narrow it. A lapsed bili_ticket (-352) or a relay rewriting the protobuf
    // is no property of the video, so a run that finds nothing is repeated as an anonymous visitor.
    struct Attempt {
        QMap<QString, QString> headers;
        QString proxy;
        const char *what;
    };
    QList<Attempt> attempts{{withWebSession(client, headers), proxyApi, "the stored session"}};
    if (const QMap<QString, QString> plain = anonymousHeaders(client);
        !proxyApi.isEmpty() || plain.value(QStringLiteral("Cookie"))
                                   != attempts.first().headers.value(QStringLiteral("Cookie")))
        attempts.append({plain, QString(), "an anonymous direct request"});

    QString refusal;
    for (qsizetype round = 0; round < attempts.size(); ++round) {
        const Attempt &attempt = attempts.at(round);
        if (client->isCancelled()) return {};
        QString why;
        QList<DanmakuComment> found =
            collectSegments(client, cid, aid, durationMs, attempt.headers, attempt.proxy, why);
        if (!found.isEmpty()) {
            if (round > 0)
                logInfo() << "Danmaku" << "cid" << cid << "answered" << attempt.what << "with"
                          << found.size() << "comments";
            return found;
        }
        if (auto legacy = fetchLegacyXml(client, cid, attempt.headers, attempt.proxy);
            !legacy.isEmpty()) {
            logInfo() << "Danmaku" << "cid" << cid << "answered on the XML endpoint:"
                      << legacy.size() << "comments";
            return legacy;
        }
        if (refusal.isEmpty()) refusal = why;
        if (!why.isEmpty())
            logWarn() << "Danmaku" << "cid" << cid << "was refused on" << attempt.what << "-" << why;
    }

    if (client->isCancelled()) return {};
    if (!refusal.isEmpty())
        logWarn() << "Danmaku" << "cid" << cid << "returned nothing:" << refusal;
    else
        logInfo() << "Danmaku" << "cid" << cid << "has no comments on either endpoint";
    return {};
}

QList<DanmakuComment> Bilibili::comments(Client *client, const QVariantMap &ids) {
    client->withSession(QStringLiteral("bilibili"));
    return fetchDanmaku(client, ids.value(QStringLiteral("cid")).toLongLong(), ids.value(QStringLiteral("aid")).toLongLong(),
                        ids.value(QStringLiteral("durationMs")).toInt(), headers(), proxyApi());
}
