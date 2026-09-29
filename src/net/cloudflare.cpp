#include "net/cloudflare.h"
#include "core/logger.h"
#include "core/settings.h"
#include "platform/platform.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QtConcurrent/QtConcurrentRun>
#include <cstring>
#include <memory>

namespace Cloudflare {

// QML images bypass Client, so a browser-bound host needs this route or posters 403.
class BrowserReply final : public QNetworkReply {
public:
    BrowserReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setOpenMode(ReadOnly);

        connect(&m_watcher, &QFutureWatcher<Cloudflare::PageFetch>::finished, this, [this] {
            if (isFinished()) return;
            const Cloudflare::PageFetch page = m_watcher.result();
            if (page.code == 200) {
                m_data = page.bytes;
                setHeader(QNetworkRequest::ContentTypeHeader, page.contentType);
                setHeader(QNetworkRequest::ContentLengthHeader, m_data.size());
                setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
                emit readyRead();
            } else {
                const auto code = page.code == 0 ? UnknownNetworkError : ContentAccessDenied;
                setError(code, page.error.isEmpty()
                                   ? QStringLiteral("browser fetch returned %1").arg(page.code)
                                   : page.error);
                emit errorOccurred(code);
            }
            setFinished(true);
            emit finished();
        });

        const QUrl url = request.url();
        QMap<QString, QString> headers;
        for (const QByteArray &header : request.rawHeaderList())
            headers[QString::fromLatin1(header)] = QString::fromUtf8(request.rawHeader(header));
        m_watcher.setFuture(QtConcurrent::run(&Cloudflare::pageFetchPool(),
            [url, headers, cancel = m_cancel] {
                return Cloudflare::fetchInBrowser(url, cancel, kSolveTimeoutMs, true, headers);
            }));
    }

    ~BrowserReply() override { m_cancel.cancel(); }

    void abort() override {
        if (isFinished()) return;
        m_cancel.cancel();
        setError(OperationCanceledError, QStringLiteral("Browser request cancelled"));
        setFinished(true);
        emit errorOccurred(OperationCanceledError);
        emit finished();
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override {
        return m_data.size() - m_read + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *out, qint64 maxSize) override {
        const qint64 size = qMin<qint64>(maxSize, m_data.size() - m_read);
        if (size <= 0) return isFinished() ? -1 : 0;
        std::memcpy(out, m_data.constData() + m_read, size_t(size));
        m_read += size;
        return size;
    }

private:
    QFutureWatcher<Cloudflare::PageFetch> m_watcher;
    CancelToken m_cancel;
    QByteArray m_data;
    qint64 m_read = 0;
};

QNetworkReply *createBrowserReply(const QNetworkRequest &request, QObject *parent) {
    return new BrowserReply(request, parent);
}

namespace {

QMutex                  g_mutex;
QSet<QString>           g_loggedBlocks;
QHash<QString, QString> g_hostUserAgents;
QSet<QString>           g_browserBoundHosts;

const CancelToken g_shutdown;

QString appFile(const char *name) {
    return Settings::dataDir() + QLatin1Char('/') + QLatin1String(name);
}

QString stripDot(QString domain) {
    if (domain.startsWith('.')) domain.remove(0, 1);
    return domain;
}

bool domainCovers(const QString &domain, const QString &host) {
    return host == domain || host.endsWith('.' + domain);
}

// Interstitial-only, so trusted at any status: a challenge can arrive as 200. Not
// `_cf_chl_opt`, which cleared pages keep.
constexpr const char *kInterstitialMarkers[] = {
    "cf-browser-verification",
    "just a moment...",
    // "/h/" is load-bearing: jsd/main.js is injected into ordinary pages too.
    "/cdn-cgi/challenge-platform/h/",
};

// Real pages carry these too, so only trust them on an already-refused status.
constexpr const char *kChallengeMarkers[] = {
    "/cdn-cgi/challenge-platform",
    "challenges.cloudflare.com/turnstile",
    "cf_chl_opt",
    "enable javascript and cookies to continue",
    "checking your browser before accessing",
};

// Visible text of an interstitial, for a page the solver has already rendered.
constexpr const char *kChallengeTextMarkers[] = {
    "just a moment",
    "checking your browser",
    "verifying you are human",
    "enable javascript and cookies to continue",
    "needs to review the security of your connection",
    "performing security verification",
};

// What a rendered firewall block says, and no challenge does: a challenge page shows a Ray ID
// and an "Attention Required!" title as well.
constexpr const char *kBlockTextMarkers[] = {
    "sorry, you have been blocked",
    "you are unable to access",
    "error 1005",           // the network's ASN is banned
    "error 1006",           // the address is banned
    "error 1020",           // a firewall rule
};

constexpr const char *kBlockMarkers[] = {
    "attention required! | cloudflare",
    "sorry, you have been blocked",
    "ray id:",
};

bool headHasAny(const QString &head, const char *const *markers, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (head.contains(QLatin1String(markers[i]))) return true;
    return false;
}

QString headerOf(const QMap<QString, QString> &headers, const char *name) {
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        if (it.key().compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return it.value();
    return {};
}

// DDoS-Guard fronts several of the same providers and refuses the same way; without it
// their 403s are never scanned and never retried in the browser.
bool servedByCloudflare(const QMap<QString, QString> &headers) {
    const QString server = headerOf(headers, "server");
    return server.contains("cloudflare", Qt::CaseInsensitive)
        || server.contains("ddos-guard", Qt::CaseInsensitive)
        || !headerOf(headers, "cf-ray").isEmpty()
        || !headerOf(headers, "x-ddg-request-id").isEmpty();
}

bool isChallenged(int code, const QMap<QString, QString> &headers, const QString &head) {
    if (headerOf(headers, "cf-mitigated").compare("challenge", Qt::CaseInsensitive) == 0) return true;
    if (!servedByCloudflare(headers)) return false;
    if (headHasAny(head, kInterstitialMarkers, std::size(kInterstitialMarkers))) return true;
    if (code != 403 && code != 503) return false;
    return headHasAny(head, kChallengeMarkers, std::size(kChallengeMarkers));
}

QString sessionKey(const QString &host, const QString &owner) {
    return (owner.isEmpty() ? providerSession(host) : owner) + '\t' + host.toLower();
}

void setHostUserAgent(const QString &host, const QString &userAgent, const QString &owner) {
    if (host.isEmpty() || userAgent.isEmpty()) return;
    QMutexLocker lock(&g_mutex);
    g_hostUserAgents[sessionKey(host, owner)] = userAgent;
}

}

QString providerSession(const QString &host) {
    const QString normalized = stripDot(host).toLower();
    for (const auto &[domain, owner] : {
             std::pair{"animepahe.pw", "animepahe"}, {"animepahe.com", "animepahe"}, {"animepahe.ru", "animepahe"},
             {"miruro.to", "miruro"}, {"anikototv.to", "anikoto"}, {"bilibili.com", "bilibili"}, {"bilivideo.com", "bilibili"}})
        if (domainCovers(QLatin1String(domain), normalized)) return QLatin1String(owner);
    // Never guess a registrable domain: co.uk would leak state.
    return normalized;
}

bool servedByProtectedEdge(const QMap<QString, QString> &headers) {
    return servedByCloudflare(headers) || !headerOf(headers, "cf-mitigated").isEmpty();
}

bool looksLikeChallengePage(const QString &text) {
    const QString head = text.left(kBodyScanBytes).toLower();
    return headHasAny(head, kChallengeTextMarkers, std::size(kChallengeTextMarkers));
}

bool looksLikeBlockPage(const QString &text) {
    const QString head = text.left(kBodyScanBytes).toLower();
    return headHasAny(head, kBlockTextMarkers, std::size(kBlockTextMarkers));
}

bool isBlocked(int code, const QMap<QString, QString> &headers, const QString &body) {
    // A rate limit (Cloudflare's 1015 page, or the site's own) is waited out, not solved: solving
    // again only spends more of the allowance. A challenge never answers 429.
    if (code == 429) return headerOf(headers, "cf-mitigated").compare("challenge", Qt::CaseInsensitive) == 0;
    const QString head = body.left(kBodyScanBytes).toLower();
    if (isChallenged(code, headers, head)) return true;
    if (code != 403 && code != 503) return false;
    return servedByCloudflare(headers) && (body.isEmpty() || headHasAny(head, kBlockMarkers, std::size(kBlockMarkers)));
}

void markBlocked(const QString &host) {
    QMutexLocker lock(&g_mutex);
    if (host.isEmpty() || g_loggedBlocks.contains(host)) return;
    g_loggedBlocks.insert(host);
    logStep() << "Cloudflare" << host << "refused the direct client";
}

bool browserBound(const QString &host, const QString &owner) {
    QMutexLocker lock(&g_mutex);
    return g_browserBoundHosts.contains(sessionKey(host, owner));
}

void markBrowserBound(const QString &host, const QString &owner) {
    QMutexLocker lock(&g_mutex);
    g_browserBoundHosts.insert(sessionKey(host, owner));
}

QThreadPool &pageFetchPool() {
    static QThreadPool pool;
    // Fetches on an already-cleared origin run side by side, so this bounds a grid of posters.
    static const bool configured = (pool.setMaxThreadCount(6), true);
    Q_UNUSED(configured);
    return pool;
}

QString hostUserAgent(const QString &host, const QString &owner) {
    // Expired cookies must not leave an unrelated browser's User-Agent behind.
    const auto cookies = CookieStore::instance().cookiesForUrl(QUrl("https://" + host + "/"), owner);
    const bool cleared = std::any_of(cookies.begin(), cookies.end(), [](const QNetworkCookie &cookie) {
        return cookie.name() == "cf_clearance" && !cookie.value().isEmpty();
    });
    if (!cleared) return {};
    QMutexLocker lock(&g_mutex);
    if (const QString exact = g_hostUserAgents.value(sessionKey(host, owner)); !exact.isEmpty()) return exact;
    for (const QNetworkCookie &cookie : cookies)
        if (cookie.name() == "cf_clearance") return g_hostUserAgents.value(sessionKey(stripDot(cookie.domain()), owner));
    return {};
}

static QString takeHeader(QMap<QString, QString> &headers, const char *name) {
    QString value;
    for (auto it = headers.begin(); it != headers.end();) {
        if (it.key().compare(QLatin1String(name), Qt::CaseInsensitive) == 0) { value = it.value(); it = headers.erase(it); }
        else ++it;
    }
    return value;
}

void applyClearanceHeaders(const QUrl &url, QMap<QString, QString> &headers, const QString &owner) {
    const QString existing = takeHeader(headers, "cookie");
    if (const QByteArray merged = CookieStore::instance().cookieHeader(url, existing, owner); !merged.isEmpty())
        headers["Cookie"] = QString::fromUtf8(merged);
    if (const QString userAgent = hostUserAgent(url.host(), owner); !userAgent.isEmpty()) {
        takeHeader(headers, "user-agent"); headers["User-Agent"] = userAgent;
    }
}

const CancelToken &shutdownToken() { return g_shutdown; }

void rememberUserAgent(const QString &host, const QString &userAgent, const QString &owner) {
    setHostUserAgent(host, userAgent, owner);
}

CookieStore &CookieStore::instance() {
    static CookieStore store;
    static const bool loaded = (store.load(), true);
    Q_UNUSED(loaded);
    return store;
}

QList<QNetworkCookie> CookieStore::cookiesForUrl(const QUrl &url, const QString &owner) const {
    QMutexLocker lock(&m_mutex);
    const auto jar = m_jars.value(owner.isEmpty() ? providerSession(url.host()) : owner);
    if (!jar) return {};
    auto cookies = jar->cookiesForUrl(url);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    cookies.removeIf([&](const QNetworkCookie &cookie) { return !cookie.isSessionCookie() && cookie.expirationDate() <= now; });
    return cookies;
}

void CookieStore::setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url, const QString &owner) {
    if (cookies.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    auto &jar = m_jars[owner.isEmpty() ? providerSession(url.host()) : owner];
    if (!jar) jar = std::make_shared<Jar>();
    const auto before = jar->allCookies();
    jar->setCookiesFromUrl(cookies, url);
    if (before == jar->allCookies()) return;
    m_pendingSave = true;
    if (QDateTime::currentMSecsSinceEpoch() - m_lastSaveMs < 5000) return;
    lock.unlock();
    save();
}

void CookieStore::invalidateClearance(const QUrl &url, const QString &owner) {
    QMutexLocker lock(&m_mutex);
    const auto jar = m_jars.value(owner.isEmpty() ? providerSession(url.host()) : owner);
    if (!jar) return;
    auto cookies = jar->allCookies();
    cookies.removeIf([&](const QNetworkCookie &cookie) {
        return cookie.name() == "cf_clearance" && domainCovers(stripDot(cookie.domain()), url.host());
    });
    jar->setAllCookies(cookies);
}

void CookieStore::insert(const QList<QNetworkCookie> &cookies, const QString &owner) {
    for (const QNetworkCookie &cookie : cookies) {
        const QUrl origin((cookie.isSecure() ? "https://" : "http://") + stripDot(cookie.domain()) + "/");
        setCookiesFromUrl({cookie}, origin, owner);
    }
}

QByteArray CookieStore::cookieHeader(const QUrl &url, const QString &existing, const QString &owner) const {
    QList<std::pair<QByteArray, QByteArray>> pairs;
    QHash<QByteArray, qsizetype> index;
    for (const QByteArray &chunk : existing.trimmed().toUtf8().split(';')) {
        const QByteArray entry = chunk.trimmed();
        const int equals = entry.indexOf('=');
        if (equals <= 0) continue;
        const QByteArray name = entry.left(equals).trimmed();
        if (index.contains(name)) pairs[index[name]].second = entry.mid(equals + 1).trimmed();
        else { index[name] = pairs.size(); pairs.append({name, entry.mid(equals + 1).trimmed()}); }
    }
    for (const QNetworkCookie &cookie : cookiesForUrl(url, owner)) {
        const auto it = index.constFind(cookie.name());
        if (it == index.constEnd()) { index[cookie.name()] = pairs.size(); pairs.append({cookie.name(), cookie.value()}); }
        else if (pairs[*it].second.isEmpty()) pairs[*it].second = cookie.value();
    }
    QByteArray header;
    for (const auto &[name, value] : std::as_const(pairs)) {
        if (!header.isEmpty()) header += "; ";
        header += name + '=' + value;
    }
    return header;
}

void CookieStore::load() {
    QList<std::pair<QString, QNetworkCookie>> restored;
    bool plain = false;
    if (QFile file(appFile("provider-cookies.json")); file.open(QIODevice::ReadOnly)) {
        // DPAPI-sealed JSON, since signed-in sessions live here; older builds wrote it in the clear.
        QByteArray json = file.readAll();
        plain = json.startsWith('[');
        if (!plain) json = Platform::unprotect(QString::fromLatin1(json)).toUtf8();
        const QJsonArray entries = QJsonDocument::fromJson(json).array();
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            for (const QNetworkCookie &cookie : QNetworkCookie::parseCookies(entry.value("cookie").toString().toUtf8()))
                restored.append({entry.value("owner").toString(), cookie});
        }
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QMutexLocker lock(&m_mutex);
    for (const auto &[owner, cookie] : restored) {
        // Clearance is bound to a fingerprint that does not survive startup.
        if (owner.isEmpty() || cookie.isSessionCookie() || cookie.expirationDate() <= now || cookie.name() == "cf_clearance") continue;
        auto &jar = m_jars[owner];
        if (!jar) jar = std::make_shared<Jar>();
        jar->setCookiesFromUrl({cookie}, QUrl((cookie.isSecure() ? "https://" : "http://") + stripDot(cookie.domain()) + "/"));
    }
    lock.unlock();
    if (plain) save();
}

void CookieStore::flush() const {
    QMutexLocker lock(&m_mutex);
    if (!m_pendingSave) return;
    lock.unlock(); save();
}

void CookieStore::save() const {
    QMutexLocker saving(&m_saveMutex);
    QJsonArray entries;
    {
        QMutexLocker lock(&m_mutex);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (auto it = m_jars.constBegin(); it != m_jars.constEnd(); ++it)
            for (const QNetworkCookie &cookie : it.value()->allCookies())
                if (!cookie.isSessionCookie() && cookie.expirationDate() > now && cookie.name() != "cf_clearance")
                    entries.append(QJsonObject{{"owner", it.key()}, {"cookie", QString::fromUtf8(cookie.toRawForm(QNetworkCookie::Full))}});
        m_pendingSave = false;
        m_lastSaveMs = QDateTime::currentMSecsSinceEpoch();
    }
    const QString sealed = Platform::protect(QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
    QSaveFile file(appFile("provider-cookies.json"));
    if (sealed.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(sealed.toLatin1()) < 0 || !file.commit()) {
        QMutexLocker lock(&m_mutex); m_pendingSave = true;
        logWarn() << "Network" << "Could not persist provider cookies";
    }
}

QList<QNetworkCookie> ProxyCookieJar::cookiesForUrl(const QUrl &url) const {
    return CookieStore::instance().cookiesForUrl(url, m_owner);
}

bool ProxyCookieJar::setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url) {
    CookieStore::instance().setCookiesFromUrl(cookies, url, m_owner);
    return true;
}

}
