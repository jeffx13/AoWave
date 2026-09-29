#include "providers/bilibili.h"
#include "core/danmakuoptions.h"
#include <QtConcurrent/QtConcurrentRun>
#include "core/exception.h"
#include "core/logger.h"
#include "core/settings.h"
#include "platform/platform.h"
#include "shows/playlistitem.h"
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
#include <QCoreApplication>
#include <memory>
#include <optional>

namespace {
const QString kCookiesKey = QStringLiteral("bilibili/cookiesEnc");
const QString kPlainCookiesGroup = QStringLiteral("bilibili/cookies");   // before sealing
// For a ticket renewed on a worker to reach the running session.
QPointer<Bilibili> g_instance;
}

QMap<QString, QString> Bilibili::storedCookies() {
    const QString json = Platform::unprotect(Settings::instance().value(kCookiesKey).toString());
    const QJsonObject jar = QJsonDocument::fromJson(json.toUtf8()).object();
    QMap<QString, QString> cookies;
    for (auto it = jar.constBegin(); it != jar.constEnd(); ++it)
        cookies.insert(it.key(), it.value().toString());
    return cookies;
}

bool Bilibili::storeCookies(const QMap<QString, QString> &cookies) {
    QJsonObject jar;
    for (auto it = cookies.constBegin(); it != cookies.constEnd(); ++it)
        if (!it.value().isEmpty()) jar.insert(it.key(), it.value());
    const QString sealed = jar.isEmpty() ? QString()
        : Platform::protect(QString::fromUtf8(QJsonDocument(jar).toJson(QJsonDocument::Compact)));
    if (!jar.isEmpty() && sealed.isEmpty()) {
        logWarn() << "Bilibili" << "could not protect the session; not storing it";
        return false;
    }
    Settings::instance().setValue(kCookiesKey, sealed);
    return true;
}

bool Bilibili::keepTicket(const QString &ticket, qint64 expiresAt) {
    QMap<QString, QString> cookies = storedCookies();
    if (cookies.isEmpty() || ticket.isEmpty()) return false;   // signed out: no jar to keep it in
    const qint64 stored = cookies.value(QStringLiteral("bili_ticket_expires")).toLongLong();
    if (stored >= expiresAt || stored > QDateTime::currentSecsSinceEpoch() + 86400) return false;
    cookies.insert(QStringLiteral("bili_ticket"), ticket);
    cookies.insert(QStringLiteral("bili_ticket_expires"), QString::number(expiresAt));
    return storeCookies(cookies);
}

Bilibili::Bilibili(QObject *parent) : ShowProvider(parent) {
    g_instance = this;
    Settings &settings = Settings::instance();
    m_proxyApi = settings.value("bilibili/proxy").toString();
    if (const auto plain = settings.groupValues(kPlainCookiesGroup); !plain.isEmpty() && storeCookies(plain))
        settings.remove(kPlainCookiesGroup);
    reloadCredentials();

    connect(&Settings::instance(), &Settings::settingsChanged, this, [this]() {
        if (reloadCredentials()) logInfo() << name() << "credentials refreshed";
    });
}

// Signing in has to take effect in the running session.
bool Bilibili::reloadCredentials() {
    QMutexLocker lock(&m_credentialsMutex);

    const QString previousCookie = m_headers.value(QStringLiteral("Cookie"));

    m_headers = {
                 {"Referer",    "https://www.bilibili.com/"},
                 {"Origin",     "https://www.bilibili.com"},
                 {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) "
                                "Gecko/20100101 Firefox/148.0"},
                 {"Accept",     "application/json, text/plain, */*"},
                 };

    const auto cookieMap = storedCookies();
    if (!cookieMap.isEmpty()) {
        // A ticket that has run out is worse than none: risk control reads it as a replayed
        // session and answers -352, which is how a jar saved months ago silently stops
        // returning danmaku. primeWebSession() mints a live one to take its place.
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
            // SESSDATA arrives percent-encoded; these two would split the header.
            QString value = it.value();
            value.replace(QLatin1Char(','), QLatin1String("%2C"));
            value.replace(QLatin1Char('*'), QLatin1String("%2A"));
            parts << (it.key() + QLatin1Char('=') + value);
        }
        // Once a run: this reloads on every change of settings. The renewed ticket is saved,
        // so it only reappears after bilibili goes unused for the ticket's three days.
        static std::atomic_bool told = false;
        if (ticketLapsed && !told.exchange(true))
            logInfo() << "Bilibili" << "the stored bili_ticket expired on"
                      << QDateTime::fromSecsSinceEpoch(ticketExpires).toString(Qt::ISODate)
                      << "- renewing and saving it";
        m_headers["Cookie"] = parts.join("; ");
        m_csrf = cookieMap.value(QStringLiteral("bili_jct"));
    } else {
        m_csrf.clear();
    }

    const QString cookie = m_headers.value(QStringLiteral("Cookie"));
    if (cookie == previousCookie && !previousCookie.isEmpty()) return false;

    if (previousCookie.isEmpty()) {
        if (cookieMap.isEmpty())
            logWarn() << "Bilibili" << "no cookies configured - member content will be unavailable";
        else
            logInfo() << "Bilibili" << "cookies loaded:" << cookieMap.size() << "entries";
    }
    m_syncRefused.store(false, std::memory_order_relaxed);
    return !previousCookie.isEmpty();
}

bool Bilibili::isSignedIn() const {
    QMutexLocker lock(&m_credentialsMutex);
    return !m_csrf.isEmpty();
}

// Provider calls run on workers while a sign-in rewrites these.
QMap<QString, QString> Bilibili::headers() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_headers;
}

QString Bilibili::csrf() const {
    QMutexLocker lock(&m_credentialsMutex);
    return m_csrf;
}

// bilibili's risk control no longer serves a bare client. x/web-interface/view answers 412
// without a signature, and the danmaku endpoints go quiet when the jar carries a bili_ticket
// that has lapsed - which is what a jar copied out of a browser months ago looks like. So the
// provider keeps its own browser-shaped session: fingerprint cookies, a ticket it renews, and
// the WBI keys signed endpoints are verified against. None of it needs an account, which is
// what makes the provider usable signed out.

namespace {

// From bilibili's own web bundle. It signs nothing secret: it only shows the request was
// built by something that read the bundle.
constexpr char kTicketHmacKey[] = "XgwSnGZ1p";

// The fixed permutation that folds img_key + sub_key into the 32-character mixin key.
constexpr int kMixinTable[] = {
    46, 47, 18,  2, 53,  8, 23, 32, 15, 50, 10, 31, 58,  3, 45, 35,
    27, 43,  5, 49, 33,  9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
    37, 48,  7, 16, 24, 55, 40, 61, 26, 17,  0,  1, 60, 51, 30,  4,
    22, 25, 54, 21, 56, 59,  6, 63, 57, 62, 11, 36, 20, 34, 44, 52,
};

// The keys rotate daily; this only bounds how long a stale pair can linger.
constexpr qint64 kWbiKeyTtlMs = 6LL * 60 * 60 * 1000;
// Otherwise an outage turns every later request into three.
constexpr qint64 kRetryAfterMs = 60000;

struct WebSession {
    QMutex  mutex;
    QString buvid3, buvid4, bNut;
    QString ticket;
    qint64  ticketExpiresAt = 0;   // seconds, as bilibili counts them
    QString imgKey, subKey;
    qint64  keysAt = 0;            // ms
    qint64  lastAttemptMs = 0;
};

WebSession &webSession() {
    static WebSession state;
    return state;
}

// ".../wbi/653657f524a547ac981ded72ea172057.png" -> "653657f524a547ac981ded72ea172057"
QString wbiKeyOf(const QString &url) {
    return url.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
}

QMap<QString, QString> bootstrapHeaders() {
    return {
        {QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; "
                                                      "rv:148.0) Gecko/20100101 Firefox/148.0")},
        {QStringLiteral("Referer"),    QStringLiteral("https://www.bilibili.com/")},
        {QStringLiteral("Origin"),     QStringLiteral("https://www.bilibili.com")},
        {QStringLiteral("Accept"),     QStringLiteral("application/json, text/plain, */*")},
    };
}

// Unsigned, no account, no bypass: these calls are what everything else is bootstrapped from,
// so they must not re-enter any of it.
Client bootstrapClient(Client *client) {
    Client scoped = client ? *client : Client({}, false);
    scoped.setVerbose(false).setBypassEnabled(false);
    return scoped.withSession(QStringLiteral("bilibili"));
}

// Caller holds the lock.
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

// Workers renew it; settings are the GUI thread's.
void keepTicketLater(const QString &ticket, qint64 expiresAt) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [ticket, expiresAt] {
        if (Bilibili::keepTicket(ticket, expiresAt) && g_instance) g_instance->reloadCredentials();
    }, Qt::QueuedConnection);
}

// Caller holds the lock. The ticket response carries the WBI keys, so one call covers both.
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

// Caller holds the lock. The ticket call usually answers with them; this is the fallback.
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

// Caller holds the lock.
QString mixinKey(const WebSession &state) {
    const QString raw = state.imgKey + state.subKey;
    if (raw.size() < 64) return {};
    QString mixed;
    mixed.reserve(32);
    for (int i = 0; i < 32; ++i) mixed.append(raw.at(kMixinTable[i]));
    return mixed;
}

// Caller holds the lock. Only the names bilibili's own bootstrap sets.
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

// The caller's jar wins name by name: a signed-in buvid3 is the one the account was seen with.
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
    // Anonymous on purpose: the ticket belongs to the session, not to an account.
    fetchTicket(client, state, QString());
    fetchWbiKeys(client, state);

    if (state.buvid3.isEmpty() || state.imgKey.isEmpty())
        logWarn() << "Bilibili" << "could not mint a browser session; risk control may refuse "
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

// QUrl's own query encoding leaves `+` and `,` alone, which a server reads back as a space
// and a separator - and, here, as a signature over a string nobody sent. This is
// encodeURIComponent, which is what bilibili signs and what the browser sends.
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
    // The four characters below are the ones encodeURIComponent leaves alone; bilibili drops
    // them from the signed value rather than let the two sides disagree about the encoding.
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
    // Even a signed-in jar needs these: the account cookies say who you are, the session ones
    // say you are a browser, and risk control wants both.
    auto requestHeaders = withWebSession(client, headers());
    if (m_proxyApi.isEmpty())
        return scoped.get(url, requestHeaders, params);

    requestHeaders["X-Proxy-Url"] = Client::urlWithParams(url, params);
    return scoped.get(m_proxyApi, requestHeaders);
}

Client::Response Bilibili::apiGetSigned(Client *client, const QString &url,
                                        QMap<QString, QString> params) const {
    const QString query = signedQuery(client, std::move(params));
    return apiGet(client, query.isEmpty() ? url : url + QLatin1Char('?') + query);
}

QList<ShowData> Bilibili::search(Client *client, const QString &query, int page, int typeIndex) {
    QString searchType = (typeIndex <= 1) ? "media_bangumi" : "media_ft";
    // Not pre-encoded: urlWithParams percent-encodes it, and the signature is computed over
    // the same decoded value the browser signs.
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
        throw AppException(tr("Bilibili's risk control turned this search away. Signing in "
                              "under Settings, Accounts usually lets it through."), name());
    const QJsonArray results = data["result"].toArray();

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
    return filterSearch(client, (typeIndex <= 1) ? 3 : 2, page, typeIndex);
}

QList<ShowData> Bilibili::latest(Client *client, int page, int typeIndex) {
    return filterSearch(client, 0, page, typeIndex);
}

QVariantMap Bilibili::filterOptions(Client *client, int typeIndex) {
    const QJsonObject data = apiGet(client, "https://api.bilibili.com/pgc/season/index/condition",
                                    {{"season_type", QString::number(kSeasonTypes[typeIndex])}, {"type", "1"}})
                                 .toJsonObject()["data"].toObject();
    // "[2010,2015)" or "[2010-01-01 00:00:00,2016-01-01 00:00:00)": a year, or a span of them.
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
            if (value == "-1") continue;   // 全部, which is no filter
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

QList<ShowData> Bilibili::filtered(Client *client, const QString & /*query*/, int page, int typeIndex,
                                   const QVariantMap &filters, bool latest) {
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
    // Series go by year, films and the rest by release date.
    if (const QString year = filters.value("year").toString(); !year.isEmpty())
        params[st == 1 || st == 4 ? "year" : "release_date"] = year;
    if (const QString status = filters.value("status").toString(); !status.isEmpty())
        params["is_finish"] = status == "finished" ? "1" : "0";

    // Encoded here: QUrlQuery leaves a year span's brackets and spaces as they are, and the index
    // answers those with a 400.
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
        // An absent part differs from an explicit ?p=1.
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

// parseUrl's links back again: a video, an episode, or "<media> <season>" and "ss <season>".
QString Bilibili::showUrl(const QString &link) const {
    if (link.startsWith(QLatin1String("BV"))) return hostUrl() + "video/" + link;
    const QStringList ids = link.split(' ');
    if (ids.size() < 2) return {};
    return hostUrl() + "bangumi/play/" + (ids[0] == QLatin1String("ep") ? "ep" : "ss") + ids[1];
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
        logWarn() << name() << "Failed to load season" << show.link;
        return 0;
    }

    const QString seasonId = QString::number(result["season_id"].toInteger());
    // Keyed by link, so a url lands on the same key search gives.
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
        int startIndex = -1;
        for (int i = 0; i < episodeList.size(); ++i) {
            auto ep = episodeList[i].toObject();
            bool isPreview = (ep["badge"].toString() == QStringLiteral("预告"));
            if (isPreview && i != episodeList.size() - 1) continue;

            QString epId = QString::number(ep["ep_id"].toInteger());
            QString link = seasonId + '&' + epId;
            // playurl is geo-blocked but the danmaku endpoint is open.
            if (const qint64 epCid = ep["cid"].toInteger(); epCid > 0) {
                link += '&' + QString::number(epCid);
                if (const qint64 aid = ep["aid"].toInteger(); aid > 0) link += '&' + QString::number(aid);
            }
            QString title = ep["title"].toString();
            QString longTitle = ep["long_title"].toString();
            if (isPreview) longTitle = QStringLiteral("(预告) ") + longTitle;

            bool ok;
            float number = title.toFloat(&ok);
            show.addEpisode(0, ok ? number : -1, link, ok ? longTitle : title, isPreview);
            if (const auto added = show.playlist()->last()) {
                added->thumbnail = ep["cover"].toString().replace(QLatin1String("http://"), QLatin1String("https://"));
                added->airedAt = ep["pub_time"].toInteger();
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

    // /pgc/view/web/season sends bare strings, the mobile endpoints {name} objects.
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
    // The unsigned x/web-interface/view answers 412 to everyone now, signed in or not; the
    // wbi/ twin behind a signature is what the site itself calls.
    const auto data = apiGetSigned(client, "https://api.bilibili.com/x/web-interface/wbi/view",
                                   {{"bvid", show.link}}).toJsonObject()["data"].toObject();
    const QJsonArray pages = data["pages"].toArray();
    if (pages.isEmpty()) {
        logWarn() << name() << "Failed to load video" << show.link;
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
    }

    if (parts.testFlag(Details)) {
        show.coverUrl    = data["pic"].toString();
        show.description = data["desc"].toString();
        show.releaseDate = QDateTime::fromSecsSinceEpoch(data["pubdate"].toInteger()).toString("yyyy-MM-dd");
        show.updateTime  = data["owner"].toObject()["name"].toString();
        show.views       = QLocale::system().toString(data["stat"].toObject()["view"].toInteger());
        if (const QString category = data["tname"].toString(); !category.isEmpty())
            show.genres.push_back(category);
    }
    return pages.size();
}

QList<VideoServer> Bilibili::loadServers(Client * /*client*/, const PlaylistItem *episode) const {
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
    auto requestHeaders = withWebSession(client, headers());
    requestHeaders["Content-Type"] = "application/x-www-form-urlencoded";
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    scoped.setVerbose(false);
    const auto response = scoped.post("https://api.bilibili.com/x/click-interface/web/heartbeat", form, requestHeaders);
    if (client->isCancelled()) return false;
    noteServerClock(response.header(QStringLiteral("Date")));
    const int code = response.toJsonObject().value("code").toInt(-1);
    if (code == 0) return true;
    // -101 not signed in, -111 csrf rejected: stale cookies.
    const bool refused = code == -101 || code == -111;
    m_syncRefused.store(refused, std::memory_order_relaxed);
    logWarn() << name() << "progress sync refused:" << (code == -1 ? response.error : QString::number(code))
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

// x/player/v2 answers per (aid, cid).
ShowProvider::RemoteProgress Bilibili::fetchProgress(Client *client, const QString &episodeLink) {
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
        logWarn() << name() << "progress read refused:" << json.value("code").toInt(-1);
        return {};
    }
    const QJsonObject data = json.value("data").toObject();
    if (!data.contains("last_play_time")) return {};

    RemoteProgress out;
    out.seconds = data.value("last_play_time").toDouble() / 1000.0;   // ms
    out.serverAt = QDateTime::currentSecsSinceEpoch() + clockSkew();
    out.valid = out.seconds > 0.0;
    return out;
}

namespace {

// Which mirrors resolve changes within minutes.
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

// Rebuilding through QUrl would re-encode the signed query.
QString hostReplaced(const QString &url, const QString &host) {
    const qsizetype from = hostStart(url);
    const qsizetype end = from < 0 ? -1 : url.indexOf(QLatin1Char('/'), from);
    if (end < 0) return url;
    return url.left(from) + host + url.mid(end);
}

// The signed path is interchangeable across UPOS mirrors only.

QStringView mirrorFamily(const QString &host) {
    const qsizetype dash = host.indexOf(QLatin1Char('-'));
    return dash <= 0 ? QStringView{} : QStringView{host}.left(dash);
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
        const QString host = hostOf(url);
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
                if (QString host = hostOf(url); !host.isEmpty() && !seen.contains(host)) {
                    seen.insert(host);
                    hosts.append(std::move(host));
                }
    return hosts;
}

// playurl picks mirrors for the relay's IP, so most do not resolve from outside China.
// Every UPOS mirror serves the same signed path, so a stream can borrow a reachable one.
class MirrorPicker {
public:
    // One batch, so the wait is one lookup. Worker thread only.
    void survey(const Client *client, const QStringList &hosts) {
        QStringList unknown;
        for (const QString &host : hosts) {
            if (const auto cached = cachedAnswer(host)) classify(host, *cached);
            else if (unknown.size() < kMaxLookups) unknown << host;
        }
        if (unknown.isEmpty() || client->isCancelled()) return;

        // Answers arrive on this thread and only while the loop runs.
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

        // A host pending at the deadline stays uncached.
        for (const QString &host : std::as_const(unknown)) {
            const auto it = answers.constFind(host);
            if (it == answers.constEnd()) { m_dead.insert(host); continue; }
            remember(host, *it);
            classify(host, *it);
        }
    }

    QString pick(const QStringList &mirrors) {
        for (const QString &url : mirrors)
            if (m_reachable.contains(hostOf(url))) return url;
        if (mirrors.isEmpty()) return {};

        const QString &original = mirrors.first();
        const QString originalHost = hostOf(original);
        const QStringView family = mirrorFamily(originalHost);
        if (family.isEmpty()) return original;
        for (const QString &host : std::as_const(m_reachable)) {
            if (mirrorFamily(host) != family) continue;
            ++m_retargeted;
            return hostReplaced(original, host);
        }
        return original;
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

    QStringList   m_reachable;
    QSet<QString> m_dead;
    int           m_retargeted = 0;
};

}

// The nesting varies with the relay.
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

static QJsonObject unwrapPlayInfo(const QJsonObject &json) {
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

// Bilibili names audio qualities by id only.
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

static QString videoCodecName(int codecid) {
    switch (codecid) {
    case 7:  return QStringLiteral("AVC");
    case 12: return QStringLiteral("HEVC");
    case 13: return QStringLiteral("AV1");
    }
    return {};
}

PlayInfo Bilibili::extractSource(Client *client, VideoServer server) {
    PlayInfo playInfo;

    const QStringList parts = server.link.split('&');
    if (parts.size() < 2) return playInfo;
    const bool upload = parts[0].startsWith(QLatin1String("BV"));
    const qint64 listedCid = upload ? parts[1].toLongLong()
                           : parts.size() > 2 ? parts[2].toLongLong() : 0;   // older links lack it
    // Season links carry the aid fourth; upload links do not.
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

    auto videoInfo = unwrapPlayInfo(json);

    const int code = json["code"].toInt(-1);
    const bool isPreviewing = videoInfo["is_preview"].toInt(0) == 1;
    const int quality = videoInfo["quality"].toInt();
    logInfo() << name() << "playurl: code=" << code
           << "preview=" << isPreviewing
           << "quality=" << quality
           << "dash=" << videoInfo.contains("dash")
           << "videos=" << videoInfo["dash"].toObject()["video"].toArray().size();

    // A trial is a 200 carrying the first few minutes, and `timelength` is then the trial's.
    if (isPreviewing) {
        logWarn() << name() << "preview/trial content returned; the account cannot watch this in full";
        throw AppException(
            isSignedIn()
                ? tr("Bilibili only offers a preview of this episode to your account. "
                     "It is member-only, region-locked, or not yet released.")
                : tr("Bilibili only offers a preview of this episode when signed out. "
                     "Sign in under Settings - Accounts to watch it in full."),
            QStringLiteral("Bilibili"));
    }
    // -101 not logged in, -403 no permission, -404 gone, -10403 region locked.
    if (code != 0 && videoInfo.isEmpty()) {
        const QString message = json["message"].toString();
        throw AppException(
            code == -101 || code == -403
                ? tr("Bilibili refused this episode to your account (%1). Sign in "
                     "under Settings - Accounts, or check the account has access.")
                      .arg(code)
                : tr("Bilibili refused this episode (%1%2). A mainland relay in "
                     "bilibili/proxy is usually what this needs.")
                      .arg(code).arg(message.isEmpty() ? QString() : QStringLiteral(": ") + message),
            QStringLiteral("Bilibili"));
    }

    // cid is the danmaku oid. Started here to overlap the mirror survey.
    if (const qint64 cid = listedCid > 0 ? listedCid : findInt(json, QLatin1String("cid")); cid > 0) {
        const int durationMs = int(findInt(json, QLatin1String("timelength")));
        const qint64 aid = listedAid > 0 ? listedAid : findInt(json, QLatin1String("aid"));
        playInfo.danmakuKey = QStringLiteral("bili-%1").arg(cid);
        // NOT client->withSession(): that client carries the race's cancel token, which fires the
        // instant a server wins, and this source is called minutes later for a prefetch.
        playInfo.danmakuSource = [worker = Client({}, false).withSession(QStringLiteral("bilibili")), cid, aid, durationMs,
                                  headers = this->headers(), proxy = m_proxyApi,
                                  started = std::make_shared<std::optional<QFuture<QList<DanmakuComment>>>>()]() {
            if (!*started)
                *started = QtConcurrent::run([worker, cid, aid, durationMs, headers, proxy]() mutable {
                    return Bilibili::fetchDanmaku(&worker, cid, aid, durationMs, headers, proxy);
                });
            return **started;
        };
        if (DanmakuOptions::current().enabled) playInfo.danmakuSource();
    }

    MirrorPicker mirrors;

    if (videoInfo.contains("dash")) {
        const QJsonObject dash = videoInfo["dash"].toObject();
        // Hi-Res is a lone object, the rest arrays.
        QJsonArray hiRes;
        if (const QJsonValue lossless = dash["flac"].toObject()["audio"]; lossless.isObject())
            hiRes.append(lossless);
        const QList<Stream> flac   = streamsOf(hiRes);
        const QList<Stream> dolby  = streamsOf(dash["dolby"].toObject()["audio"].toArray());
        const QList<Stream> audios = streamsOf(dash["audio"].toArray());
        const QList<Stream> videos = streamsOf(dash["video"].toArray());

        // Surveyed before any is picked, so a stream can borrow another's mirror.
        mirrors.survey(client, distinctHosts({&videos, &audios, &dolby, &flac}));

        auto addAudio = [&](const Stream &stream, const QString &fallbackName) {
            const int bw = stream.json["bandwidth"].toInt();
            if (const QString url = mirrors.pick(stream.mirrors); !url.isEmpty())
                playInfo.audios.emplaceBack(url, audioQualityName(stream.json["id"].toInt(), fallbackName), "", bw);
        };
        for (const Stream &stream : flac)   addAudio(stream, QStringLiteral("Hi-Res无损"));
        for (const Stream &stream : dolby)  addAudio(stream, QStringLiteral("杜比全景声"));
        for (const Stream &stream : audios) addAudio(stream, Track::formatBitrate(stream.json["bandwidth"].toInt()));

        QHash<int, QString> qualityNames;
        for (const QJsonValue &v : videoInfo["support_formats"].toArray()) {
            const QJsonObject format = v.toObject();
            qualityNames.insert(format["quality"].toInt(), format["new_description"].toString());
        }

        // Bilibili sorts best-first, one entry per codec.
        for (const Stream &stream : videos) {
            const int h  = stream.json["height"].toInt();
            const int bw = stream.json["bandwidth"].toInt();
            QString label = qualityNames.value(stream.json["id"].toInt());
            if (label.isEmpty()) label = QString("%1p").arg(h);
            if (const QString codec = videoCodecName(stream.json["codecid"].toInt()); !codec.isEmpty())
                label += QStringLiteral(" · ") + codec;
            if (const QString url = mirrors.pick(stream.mirrors); !url.isEmpty())
                playInfo.videos.emplaceBack(url, label, h, bw);
        }
    }
    else if (videoInfo.contains("durl")) {
        const QList<Stream> durl = streamsOf(videoInfo["durl"].toArray());
        mirrors.survey(client, distinctHosts({&durl}));
        for (const Stream &stream : durl) {
            if (const QString url = mirrors.pick(stream.mirrors); !url.isEmpty())
                playInfo.videos.emplaceBack(url, QStringLiteral("Q%1 (%2 bytes)")
                    .arg(videoInfo["quality"].toInt()).arg(stream.json["size"].toInt()));
        }
    } else if (videoInfo.contains("durls")) {
        // at(), not [0]: the non-const operator[] asserts.
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
        mirrors.survey(client, distinctHosts({&durls}));
        for (qsizetype i = 0; i < durls.size(); ++i) {
            if (const QString url = mirrors.pick(durls[i].mirrors); !url.isEmpty())
                playInfo.videos.emplaceBack(url, QStringLiteral("Q%1 (%2 bytes)")
                    .arg(qualities[i]).arg(durls[i].json["size"].toInt()));
        }
    } else {
        logWarn() << name() << "No video streams found in response";
        return playInfo;
    }

    logInfo() << name() << "Extracted:" << playInfo.videos.size() << "video,"
           << playInfo.audios.size() << "audio streams;" << "mirrors"
           << mirrors.reachable() << "reachable," << mirrors.dead() << "unresolvable";
    if (mirrors.retargeted() > 0)
        logWarn() << name() << mirrors.retargeted()
                  << "stream(s) had no reachable mirror of their own and were retargeted";
    if (mirrors.reachable() == 0)
        logWarn() << name() << "no playurl mirror resolves - check DNS or the bilibili/proxy relay";

    playInfo.addHeader("Referer", "https://www.bilibili.com/");
    playInfo.addHeader("User-Agent", headers().value("User-Agent"));
    return playInfo;
}

namespace {

// The web client's own endpoints: no signing, no cookies, no referer.
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

// SESSDATA, bili_jct and DedeUserID: all three or nothing.
const QStringList &requiredCookies() {
    static const QStringList names{QStringLiteral("SESSDATA"), QStringLiteral("bili_jct"),
                                   QStringLiteral("DedeUserID")};
    return names;
}

}

Bilibili::LoginTicket Bilibili::beginLogin(Client *client) {
    if (!client) return {};
    // Bypass off: these answer plain JSON.
    Client scoped = client->withSession(QStringLiteral("bilibili"));
    const auto response = scoped.setBypassEnabled(false).get(QLatin1String(kGenerate), loginHeaders());

    const QJsonObject json = response.toJsonObject();
    if (response.code != 200 || json.value(QStringLiteral("code")).toInt(-1) != 0) {
        logWarn() << "Bilibili" << "could not start a sign-in:" << response.code
                  << json.value(QStringLiteral("message")).toString();
        return {};
    }

    const QJsonObject data = json.value(QStringLiteral("data")).toObject();
    LoginTicket ticket;
    ticket.qrcodeKey  = data.value(QStringLiteral("qrcode_key")).toString();
    ticket.confirmUrl = QUrl(data.value(QStringLiteral("url")).toString());
    if (!ticket.isValid()) logWarn() << "Bilibili" << "the sign-in ticket came back incomplete";
    return ticket;
}

Bilibili::LoginPoll Bilibili::pollLogin(Client *client, const LoginTicket &ticket) {
    LoginPoll result;
    if (!client || !ticket.isValid()) { result.status = LoginStatus::Failed; return result; }

    Client scoped = client->withSession(QStringLiteral("bilibili"));
    const auto response = scoped.setBypassEnabled(false).get(
        QLatin1String(kPoll), loginHeaders(),
        {{QStringLiteral("qrcode_key"), ticket.qrcodeKey}});

    const QJsonObject json = response.toJsonObject();
    if (response.code != 200 || json.isEmpty()) { result.status = LoginStatus::Failed; return result; }

    const QJsonObject data = json.value(QStringLiteral("data")).toObject();
    // The outer `code` is the request; this one is the sign-in.
    switch (data.value(QStringLiteral("code")).toInt(-1)) {
    case 86101: result.status = LoginStatus::Pending; return result;   // not opened yet
    case 86090: result.status = LoginStatus::Scanned; return result;   // opened, not confirmed
    case 86038: result.status = LoginStatus::Expired; return result;   // the ticket timed out
    case 0:     break;
    default:    result.status = LoginStatus::Failed;  return result;
    }

    // A cross-domain redirect carrying the session in its query string.
    const QUrlQuery query(QUrl(data.value(QStringLiteral("url")).toString()));
    for (const auto &item : query.queryItems(QUrl::FullyDecoded))
        if (!item.second.isEmpty()) result.cookies.insert(item.first, item.second);

    for (const QString &name : requiredCookies()) {
        if (result.cookies.value(name).isEmpty()) {
            logWarn() << "Bilibili" << "the confirmation carried no" << name;
            result.status = LoginStatus::Failed;
            result.cookies.clear();
            return result;
        }
    }
    result.status = LoginStatus::Confirmed;
    return result;
}

QMap<QString, QString> Bilibili::parseCookieString(const QString &text) {
    QMap<QString, QString> cookies;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return cookies;

    // Semicolons in a header, newlines in a DevTools copy, a stray "Cookie:" prefix.
    QString body = trimmed;
    if (body.startsWith(QLatin1String("Cookie:"), Qt::CaseInsensitive)) body.remove(0, 7);
    const auto parts = body.split(QRegularExpression(QStringLiteral("[;\\n\\r\\t]")),
                                  Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int split = part.indexOf(QLatin1Char('='));
        if (split <= 0) continue;
        const QString name  = part.left(split).trimmed();
        const QString value = part.mid(split + 1).trimmed();
        if (!name.isEmpty() && !value.isEmpty()) cookies.insert(name, value);
    }

    for (const QString &name : requiredCookies())
        if (cookies.value(name).isEmpty()) return {};
    return cookies;
}
