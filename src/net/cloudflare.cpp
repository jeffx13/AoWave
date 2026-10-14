#include "net/cloudflare.h"
#include "net/client.h"
#include "app/logger.h"
#include "app/settings.h"
#include "system/platform.h"
#include <QApplication>
#include <QAtomicInteger>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJSEngine>
#include <QMutex>
#include <QNetworkReply>
#include <QPointer>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVarLengthArray>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineProfileBuilder>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <cstring>
#include <functional>
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

struct Site {
    const char *domain;
    const char *owner;
    bool        cloudflare;
};
constexpr Site kSites[] = {
    {"animepahe.pw", "animepahe", true}, {"miruro.to", "miruro", true}, {"anikototv.to", "anikoto", true},
    {"bilibili.com", "bilibili", false}, {"bilivideo.com", "bilibili", false},
};

// Interstitial-only, so trusted at any status: a challenge can arrive as 200.
constexpr const char *kInterstitialMarkers[] = {
    "cf-browser-verification",
    "just a moment...",
    // "/h/" is load-bearing: jsd/main.js is injected into ordinary pages too.
    "/cdn-cgi/challenge-platform/h/",
};

constexpr const char *kChallengeMarkers[] = {
    "/cdn-cgi/challenge-platform",
    "challenges.cloudflare.com/turnstile",
    "cf_chl_opt",
    "enable javascript and cookies to continue",
    "checking your browser before accessing",
};

constexpr const char *kChallengeTextMarkers[] = {
    "just a moment",
    "checking your browser",
    "verifying you are human",
    "enable javascript and cookies to continue",
    "needs to review the security of your connection",
    "performing security verification",
};

constexpr const char *kBlockTextMarkers[] = {
    "sorry, you have been blocked",
    "you are unable to access",
    "error 1005",
    "error 1006",
    "error 1020",
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

// DDoS-Guard fronts several of these providers; without it their 403s are never retried in the browser.
bool servedByCloudflare(const QMap<QString, QString> &headers) {
    const QString server = Client::headerValue(headers, "server");
    return server.contains("cloudflare", Qt::CaseInsensitive)
        || server.contains("ddos-guard", Qt::CaseInsensitive)
        || !Client::headerValue(headers, "cf-ray").isEmpty()
        || !Client::headerValue(headers, "x-ddg-request-id").isEmpty();
}

bool isChallenged(int code, const QMap<QString, QString> &headers, const QString &head) {
    if (Client::headerValue(headers, "cf-mitigated").compare("challenge", Qt::CaseInsensitive) == 0) return true;
    if (!servedByCloudflare(headers)) return false;
    if (headHasAny(head, kInterstitialMarkers, std::size(kInterstitialMarkers))) return true;
    if (code != 403 && code != 503) return false;
    return headHasAny(head, kChallengeMarkers, std::size(kChallengeMarkers));
}

QString sessionKey(const QString &host, const QString &owner) {
    return (ownerOf(host, owner)) + '\t' + host.toLower();
}

void setHostUserAgent(const QString &host, const QString &userAgent, const QString &owner) {
    if (host.isEmpty() || userAgent.isEmpty()) return;
    QMutexLocker lock(&g_mutex);
    g_hostUserAgents[sessionKey(host, owner)] = userAgent;
}

QString issuedUserAgent(const QString &host, const QString &owner) {
    QMutexLocker lock(&g_mutex);
    return g_hostUserAgents.value(sessionKey(host, owner));
}

}

QString providerSession(const QString &host) {
    const QString normalized = stripDot(host).toLower();
    for (const Site &site : kSites)
        if (domainCovers(QLatin1String(site.domain), normalized)) return QLatin1String(site.owner);
    // Never guess a registrable domain: co.uk would leak state.
    return normalized;
}

bool behindCloudflare(const QString &host) {
    const QString normalized = stripDot(host).toLower();
    for (const Site &site : kSites)
        if (domainCovers(QLatin1String(site.domain), normalized)) return site.cloudflare;
    return !hostUserAgent(host).isEmpty();
}

bool servedByProtectedEdge(const QMap<QString, QString> &headers) {
    return servedByCloudflare(headers) || !Client::headerValue(headers, "cf-mitigated").isEmpty();
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
    if (code == 429) return Client::headerValue(headers, "cf-mitigated").compare("challenge", Qt::CaseInsensitive) == 0;
    const QString head = body.left(kBodyScanBytes).toLower();
    if (isChallenged(code, headers, head)) return true;
    if (code != 403 && code != 503) return false;
    return servedByCloudflare(headers) && (body.isEmpty() || headHasAny(head, kBlockMarkers, std::size(kBlockMarkers)));
}

void markBlocked(const QString &host) {
    QMutexLocker lock(&g_mutex);
    if (host.isEmpty() || g_loggedBlocks.contains(host)) return;
    g_loggedBlocks.insert(host);
    logStep() << "Web" << QStringLiteral("Cloudflare:") << host << "refused the direct client";
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
    static const bool configured = (pool.setMaxThreadCount(6), true);
    Q_UNUSED(configured);
    return pool;
}

QString hostUserAgent(const QString &host, const QString &owner) {
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

Clearances &Clearances::instance() {
    static Clearances clearances;
    return clearances;
}

Clearances *Clearances::create(QQmlEngine *, QJSEngine *) {
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

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
    const auto jar = m_jars.value(ownerOf(url.host(), owner));
    if (!jar) return {};
    auto cookies = jar->cookiesForUrl(url);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    cookies.removeIf([&](const QNetworkCookie &cookie) { return !cookie.isSessionCookie() && cookie.expirationDate() <= now; });
    return cookies;
}

void CookieStore::setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url, const QString &owner) {
    if (cookies.isEmpty()) return;
    QMutexLocker lock(&m_mutex);
    auto &jar = m_jars[ownerOf(url.host(), owner)];
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
    const auto jar = m_jars.value(ownerOf(url.host(), owner));
    if (!jar) return;
    auto cookies = jar->allCookies();
    if (!cookies.removeIf([&](const QNetworkCookie &cookie) {
            return cookie.name() == "cf_clearance" && domainCovers(stripDot(cookie.domain()), url.host());
        }))
        return;
    jar->setAllCookies(cookies);
    m_pendingSave = true;
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
    if (QFile file(appFile("provider-cookies.json")); file.open(QIODevice::ReadOnly)) {
        const QByteArray json = Platform::unprotect(QString::fromLatin1(file.readAll())).toUtf8();
        const QJsonArray entries = QJsonDocument::fromJson(json).array();
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            const QString owner = entry.value("owner").toString();
            const QString userAgent = entry.value("ua").toString();
            for (const QNetworkCookie &cookie : QNetworkCookie::parseCookies(entry.value("cookie").toString().toUtf8())) {
                // A clearance only works with the User-Agent it was issued to.
                if (cookie.name() == "cf_clearance") {
                    if (userAgent.isEmpty()) continue;
                    setHostUserAgent(stripDot(cookie.domain()), userAgent, owner);
                }
                restored.append({owner, cookie});
            }
        }
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QMutexLocker lock(&m_mutex);
    for (const auto &[owner, cookie] : restored) {
        if (owner.isEmpty() || cookie.isSessionCookie() || cookie.expirationDate() <= now) continue;
        auto &jar = m_jars[owner];
        if (!jar) jar = std::make_shared<Jar>();
        jar->setCookiesFromUrl({cookie}, QUrl((cookie.isSecure() ? "https://" : "http://") + stripDot(cookie.domain()) + "/"));
    }
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
        // A session cookie says nothing of its age, so a clearance of that kind stays in the web engine's profile.
        for (auto it = m_jars.constBegin(); it != m_jars.constEnd(); ++it)
            for (const QNetworkCookie &cookie : it.value()->allCookies()) {
                if (cookie.isSessionCookie() || cookie.expirationDate() <= now) continue;
                QJsonObject entry{{"owner", it.key()}, {"cookie", QString::fromUtf8(cookie.toRawForm(QNetworkCookie::Full))}};
                if (cookie.name() == "cf_clearance") {
                    const QString userAgent = issuedUserAgent(stripDot(cookie.domain()), it.key());
                    if (userAgent.isEmpty()) continue;
                    entry.insert("ua", userAgent);
                }
                entries.append(entry);
            }
        m_pendingSave = false;
        m_lastSaveMs = QDateTime::currentMSecsSinceEpoch();
    }
    const QString sealed = Platform::protect(QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
    QSaveFile file(appFile("provider-cookies.json"));
    if (sealed.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(sealed.toLatin1()) < 0 || !file.commit()) {
        QMutexLocker lock(&m_mutex); m_pendingSave = true;
        logWarn() << "Web" << "Could not persist provider cookies";
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

namespace {

using namespace Cloudflare;

constexpr int    kIdleSweepMs         = 30000;
constexpr qint64 kIdleCloseMs         = 90000;
constexpr qint64 kMaxBrowserBodyBytes = 32LL * 1024 * 1024;
constexpr int    kReloadAtPercent     = 55;
constexpr qint64 kHumanAfterMs        = 15000;
constexpr qint64 kHumanTimeoutMs      = 180000;

constexpr const char *kFallbackUserAgent = kChromeUserAgent;

QString disguisedUserAgent(const QString &defaultAgent) {
    if (defaultAgent.isEmpty()) return QString::fromLatin1(kFallbackUserAgent);
    static const QRegularExpression qtToken(QStringLiteral("\\s*QtWebEngine/[^\\s)]+"));
    QString agent = defaultAgent;
    agent.remove(qtToken);
    return agent.simplified();
}

// The profile replays its stored cookies on every start, so only a clearance the mirror lacks is news.
void mirrorCookie(const QNetworkCookie &cookie, const QString &owner, const QString &agent) {
    CookieStore &store = CookieStore::instance();
    if (cookie.name() != "cf_clearance" || cookie.value().isEmpty()) {
        store.insert({cookie}, owner);
        return;
    }
    const QString domain = stripDot(cookie.domain());
    const QList<QNetworkCookie> held = store.cookiesForUrl(QUrl("https://" + domain + "/"), owner);
    const bool known = std::any_of(held.begin(), held.end(), [&](const QNetworkCookie &have) {
        return have.name() == cookie.name() && have.value() == cookie.value();
    });
    // First: the save the insert may trigger writes the clearance with the User-Agent it was issued to.
    if (!known || issuedUserAgent(domain, owner).isEmpty()) rememberUserAgent(domain, agent, owner);
    store.insert({cookie}, owner);

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (known || (!cookie.isSessionCookie() && cookie.expirationDate() <= now)) return;
    logInfo() << "Web" << QStringLiteral("Cloudflare:") << owner << "earned a clearance for" << domain << "-"
              << (cookie.isSessionCookie()
                      ? QStringLiteral("a session cookie")
                      : QStringLiteral("expires %1, in %2 min").arg(cookie.expirationDate().toUTC().toString(Qt::ISODate))
                                                               .arg(now.secsTo(cookie.expirationDate()) / 60));
    Clearances::instance().earned();
}

std::atomic_bool g_engineRan{false};
std::atomic_bool g_engineBroken{false};

thread_local bool g_lastWasRefusal = false;
enum class HumanCheck { None, Passed, Closed, TimedOut };
thread_local HumanCheck g_lastHumanCheck = HumanCheck::None;

QString jsLiteral(const QJsonValue &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
        .mid(1).chopped(1);
}

// Cloudflare issues the clearance on an ordinary page; a JSON endpoint answers a document navigation with a bare 403.
QUrl originRoot(const QUrl &url) {
    QUrl root;
    root.setScheme(url.scheme());
    root.setHost(url.host());
    if (url.port() != -1) root.setPort(url.port());
    root.setPath(QStringLiteral("/"));
    return root;
}

// One lock per provider: navigating is exclusive, an in-page fetch on a settled origin shared.
// Immortal on purpose: destroying one at static teardown while a worker holds it is undefined.
QReadWriteLock *laneFor(const QString &owner) {
    static QMutex mutex;
    static QHash<QString, QReadWriteLock *> lanes;
    QMutexLocker lock(&mutex);
    QReadWriteLock *&entry = lanes[owner];
    if (!entry) entry = new QReadWriteLock;
    return entry;
}

int waitSliceMs(const QDeadlineTimer &deadline) {
    const qint64 remaining = deadline.remainingTime();
    if (remaining < 1) return 1;
    return remaining > 50 ? 50 : int(remaining);
}

class Lane {
public:
    enum Mode { Shared, Exclusive };

    Lane(const QString &owner, CancelToken cancel, const QDeadlineTimer &deadline)
        : m_lock(laneFor(owner)), m_cancel(std::move(cancel)), m_deadline(deadline) {}
    ~Lane() { release(); }
    Lane(const Lane &) = delete;
    Lane &operator=(const Lane &) = delete;

    bool acquire(Mode mode) {
        release();
        while (!m_cancel.isCancelled() && !m_deadline.hasExpired()) {
            const int slice = waitSliceMs(m_deadline);
            if (mode == Shared ? m_lock->tryLockForRead(slice) : m_lock->tryLockForWrite(slice)) {
                m_held = true;
                return true;
            }
        }
        return false;
    }

    void release() {
        if (!m_held) return;
        m_held = false;
        m_lock->unlock();
    }

private:
    QReadWriteLock *m_lock;
    CancelToken     m_cancel;
    QDeadlineTimer  m_deadline;
    bool            m_held = false;
};

class CheckWindow final : public QWebEngineView {
public:
    explicit CheckWindow(QWebEnginePage *page) {
        setPage(page);
        resize(520, 620);
        setAttribute(Qt::WA_QuitOnClose, false);
    }
    void ask(const QString &host, std::function<void()> onClosed) {
        m_onClosed = std::move(onClosed);
        setWindowTitle(QStringLiteral("%1 wants to check you're human").arg(host));
        show();
        raise();
        activateWindow();
    }
    void finish() { m_onClosed = nullptr; }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (const auto onClosed = std::exchange(m_onClosed, nullptr)) onClosed();
        QWebEngineView::closeEvent(event);
    }

private:
    std::function<void()> m_onClosed;
};

struct Session {
    QWebEngineProfile *profile = nullptr;
    QWebEnginePage    *page    = nullptr;
    QPointer<CheckWindow> check;
    QString userAgent;
    QUrl    settledUrl;
    quint64 navigation = 0;
    qint64  lastUsedMs = 0;

    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    // Qt owns the render process, so closing a session is a delete; the profile must outlive its pages.
    ~Session() {
        if (check) { check->finish(); delete check.data(); }
        delete page;
        delete profile;
    }
};

class Host : public QObject {
public:
    static Host &instance() {
        static Host *host = [] {
            auto *created = new Host;
            created->moveToThread(QCoreApplication::instance()->thread());
            return created;
        }();
        return *host;
    }

    Session *find(const QString &owner) {
        const auto it = m_sessions.constFind(owner);
        return it == m_sessions.constEnd() ? nullptr : it->get();
    }

    Session &session(const QString &owner) {
        std::shared_ptr<Session> &slot = m_sessions[owner];
        if (slot) {
            slot->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            return *slot;
        }
        slot = std::make_shared<Session>();
        Session &entry = *slot;

        // Kept on disk (cf_clearance is bound to this profile); the builder sets the paths, as the constructor makes defaults in %APPDATA% first.
        entry.profile = QWebEngineProfileBuilder()
                            .setPersistentStoragePath(Settings::dataDir() + QStringLiteral("/browser/") + owner)
                            .setCachePath(Settings::tempDir() + QStringLiteral("/browser/") + owner)
                            .setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies)
                            .createProfile(QStringLiteral("aowave-") + owner);
        entry.userAgent = disguisedUserAgent(entry.profile->httpUserAgent());
        entry.profile->setHttpUserAgent(entry.userAgent);
        entry.profile->setHttpAcceptLanguage(QStringLiteral("en-US,en;q=0.9"));

        QWebEngineCookieStore *jar = entry.profile->cookieStore();
        const QString agent = entry.userAgent;
        connect(jar, &QWebEngineCookieStore::cookieAdded, this,
                [owner, agent](const QNetworkCookie &cookie) { mirrorCookie(cookie, owner, agent); });
        jar->loadAllCookies();

        entry.page = new QWebEnginePage(entry.profile);
        QWebEngineSettings *settings = entry.page->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
        settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
        settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
        settings->setAttribute(QWebEngineSettings::WebGLEnabled, true);
        settings->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, false);
        entry.page->setBackgroundColor(Qt::transparent);

        entry.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
        startIdleSweep();
        return entry;
    }

    void closeAll() { m_sessions.clear(); }

private:
    Host() = default;

    void startIdleSweep() {
        if (m_idle) return;
        m_idle = new QTimer(this);
        m_idle->setInterval(kIdleSweepMs);
        connect(m_idle, &QTimer::timeout, this, [this]() { closeIdle(); });
        m_idle->start();
    }

    void closeIdle() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (now - (*it)->lastUsedMs < kIdleCloseMs) { ++it; continue; }
            QReadWriteLock *lane = laneFor(it.key());
            if (!lane->tryLockForWrite()) { ++it; continue; }
            logInfo() << "Web" << QStringLiteral("Cloudflare:") << "closing the idle" << it.key() << "web engine session";
            it = m_sessions.erase(it);
            lane->unlock();
        }
    }

    QHash<QString, std::shared_ptr<Session>> m_sessions;
    QTimer *m_idle = nullptr;
};

// False if the deadline passed or the request was cancelled; `task` may still run, so touch only what outlives the call.
bool runOnGui(std::function<void()> task, const QDeadlineTimer &deadline,
              const CancelToken &cancel) {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
        task();
        return true;
    }
    auto done = std::make_shared<QSemaphore>();
    QMetaObject::invokeMethod(&Host::instance(), [task = std::move(task), done]() {
        task();
        done->release();
    }, Qt::QueuedConnection);
    while (!deadline.hasExpired() && !cancel.isCancelled())
        if (done->tryAcquire(1, waitSliceMs(deadline))) return true;
    return false;
}

struct FetchState {
    QSemaphore done;
    QMutex mutex;
    QString payload;
};

void pollFor(QWebEnginePage *page, const QString &probe, const std::shared_ptr<FetchState> &state,
             qint64 deadlineMs, int attempt = 0) {
    if (QDateTime::currentMSecsSinceEpoch() > deadlineMs) { state->done.release(); return; }
    page->runJavaScript(probe, [page, probe, state, deadlineMs, attempt](const QVariant &value) {
        const QString payload = value.toString();
        if (!payload.isEmpty()) {
            QMutexLocker lock(&state->mutex);
            state->payload = payload;
            state->done.release();
            return;
        }
        const int wait = attempt < 50 ? 10 : 50;
        QTimer::singleShot(wait, page, [page, probe, state, deadlineMs, attempt]() {
            pollFor(page, probe, state, deadlineMs, attempt + 1);
        });
    });
}

void pollFetched(QWebEnginePage *page, const std::shared_ptr<FetchState> &state, quint64 id,
                 qint64 deadlineMs) {
    pollFor(page,
            QStringLiteral("(function(){var s=window.__aoFetch;if(!s)return '';var v=s['%1'];"
                           "if(!v)return '';delete s['%1'];return v;})()").arg(id),
            state, deadlineMs);
}

struct SettleState {
    QSemaphore done;
    std::atomic_bool ok{false};
    std::atomic_bool refused{false};
    bool reloaded = false;
    std::atomic<qint64> deadlineMs{0};
    qint64 challengedSinceMs = 0;
    std::atomic_bool askedHuman{false};
    std::atomic_bool closedByUser{false};
    QPointer<QTimer> watcher;
    bool finished = false;
    qint64 probeSentMs = 0;

    void note(const QString &text) { QMutexLocker lock(&m_noteMutex); m_note = text; }
    QString lastNote() const {
        QMutexLocker lock(&m_noteMutex);
        return m_note.isEmpty() ? QStringLiteral("the page never reported a finished load - "
                                                 "the web engine may not have started")
                                : m_note;
    }

private:
    mutable QMutex m_noteMutex;
    QString m_note;
};

void finishSettle(const std::shared_ptr<SettleState> &state) {
    if (state->finished) return;
    state->finished = true;
    if (state->watcher) state->watcher->deleteLater();
    state->done.release();
}

bool askHuman(Session *entry, const QString &host, const std::shared_ptr<SettleState> &state) {
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()) || Platform::quiet()) return false;
    if (!entry->check) entry->check = new CheckWindow(entry->page);
    entry->check->ask(host, [state] {
        state->closedByUser = true;
        finishSettle(state);
    });
    logInfo() << "Web" << QStringLiteral("Cloudflare:") << host << "wants a person to confirm they are human; asking";
    return true;
}

void closeCheck(Session *entry) {
    if (!entry || !entry->check) return;
    entry->check->finish();
    entry->check->hide();
}

// A timer, not chained replies: a navigation drops a pending runJavaScript reply, and clearing navigates.
void watchSettle(QWebEnginePage *page, const std::shared_ptr<SettleState> &state,
                 const QString &owner, const QString &expectedOwner, quint64 navigation,
                 qint64 reloadAtMs) {
    static const QString probe = QStringLiteral(
        "(function(){var done=document.readyState==='complete';"
        "var cf=!!document.querySelector("
        "'#challenge-running,#challenge-stage,#cf-challenge-running,#challenge-form,#cf-chl-widget');"
        "return JSON.stringify({origin:location.origin,ready:document.readyState,"
        "title:document.title||'',cf:cf,"
        "text:(done&&document.body)?document.body.textContent.slice(0,4096):''});})()");

    const auto answer = [page, state, owner, expectedOwner, navigation, reloadAtMs](const QVariant &value) {
        state->probeSentMs = 0;
        if (state->finished) return;
        Session *entry = Host::instance().find(owner);
        if (!entry || entry->navigation != navigation) { finishSettle(state); return; }

        const QJsonObject probed = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        if (probed.isEmpty()) {
            state->note(QStringLiteral("the page did not answer the probe script"));
            return;
        }
        g_engineRan.store(true);
        const QString origin = probed.value("origin").toString();
        const QString title  = probed.value("title").toString();
        const QString ready  = probed.value("ready").toString();
        const QString host   = QUrl(origin).host();
        const bool challenged = probed.value("cf").toBool() || looksLikeChallengePage(title);
        const bool blockPage  = looksLikeBlockPage(title)
                             || looksLikeBlockPage(probed.value("text").toString());
        state->note(QStringLiteral("origin %1, readyState %2, challenge %3, title \"%4\"")
                        .arg(origin.isEmpty() ? QStringLiteral("(none)") : origin,
                             ready, challenged ? QStringLiteral("yes") : QStringLiteral("no"),
                             title));

        const bool arrived = !host.isEmpty() && providerSession(host) == expectedOwner;
        if (arrived && ready == QLatin1String("complete") && !challenged && !blockPage) {
            entry->settledUrl = page->url();
            entry->lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            state->ok = true;
            closeCheck(entry);
            finishSettle(state);
            return;
        }
        if (!host.isEmpty() && !arrived) { finishSettle(state); return; }
        if (blockPage) {
            state->refused = true;
            finishSettle(state);
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (arrived && challenged) {
            if (!state->challengedSinceMs) state->challengedSinceMs = now;
            if (!state->askedHuman && now - state->challengedSinceMs > kHumanAfterMs
                && askHuman(entry, host, state)) {
                state->askedHuman = true;
                state->deadlineMs = now + kHumanTimeoutMs;
            }
        }
        if (!state->reloaded && !state->askedHuman && now > reloadAtMs) {
            state->reloaded = true;
            page->triggerAction(QWebEnginePage::Reload);
        }
    };

    auto *timer = new QTimer(page);
    state->watcher = timer;
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, page, [page, state, answer]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now > state->deadlineMs.load()) { finishSettle(state); return; }
        if (state->probeSentMs && now - state->probeSentMs < 2000) return;
        state->probeSentMs = now;
        page->runJavaScript(probe, QWebEngineScript::ApplicationWorld, answer);
    });
    timer->start();
}

class Browser {
public:
    static bool available() {
        return QCoreApplication::instance() != nullptr && !g_engineBroken.load();
    }

    static PageFetch fetch(const QUrl &url, const CancelToken &requestCancel, int timeoutMs, bool bytes,
                           const QMap<QString, QString> &headers, const QString &owner) {
        const CancelToken cancel = requestCancel.composeWith(shutdownToken());
        QDeadlineTimer deadline(qMax(1, timeoutMs));
        const QUrl root = originRoot(url);
        Lane lane(owner, cancel, deadline);
        PageFetch page;
        g_lastWasRefusal = false;

        bool refused = false;
        if (lane.acquire(Lane::Shared) && settledOn(owner, root, cancel, deadline)) {
            page = fetchOnce(url, cancel, deadline, bytes, headers, owner);
            if (!challenged(page, bytes)) return report(std::move(page), url, bytes, owner);
            refused = true;
        }
        lane.release();

        QVarLengthArray<QUrl, 2> documents;
        documents.append(root);
        if (url != root) documents.append(url);

        if (!lane.acquire(Lane::Exclusive)) {
            if (page.error.isEmpty())
                page.error = QStringLiteral("The web engine request was cancelled while queued");
            return page;
        }
        for (const QUrl &document : documents) {
            if (refused) CookieStore::instance().invalidateClearance(url, owner);
            const bool ready = !refused && document == root && settledOn(owner, root, cancel, deadline);
            if (!ready && !settle(document, cancel, deadline, owner)) {
                if (g_lastWasRefusal || g_lastHumanCheck != HumanCheck::None) break;
                refused = false;
                continue;
            }
            page = fetchOnce(url, cancel, deadline, bytes, headers, owner);
            if (!challenged(page, bytes)) break;
            refused = true;
        }
        return report(std::move(page), url, bytes, owner);
    }

    static QString solve(const QUrl &url, const CancelToken &requestCancel, int timeoutMs,
                         const QString &owner) {
        const CancelToken cancel = requestCancel.composeWith(shutdownToken());
        QDeadlineTimer deadline(qMax(1, timeoutMs));
        const QUrl root = originRoot(url);
        Lane lane(owner, cancel, deadline);
        g_lastWasRefusal = false;
        if (!lane.acquire(Lane::Exclusive)) return {};

        if (!settle(root, cancel, deadline, owner)
            && (url == root || !settle(url, cancel, deadline, owner)))
            return {};
        const QString agent = sessionUserAgent(owner, cancel, deadline);
        if (!agent.isEmpty()) rememberUserAgent(url.host(), agent, owner);
        return agent;
    }

private:
    static bool challenged(const PageFetch &page, bool bytes) {
        return isBlocked(page.code, page.headers,
                         bytes ? QString::fromUtf8(page.bytes.left(kBodyScanBytes)) : page.body);
    }

    static PageFetch report(PageFetch page, const QUrl &url, bool bytes, const QString &owner) {
        const bool blocked = challenged(page, bytes);
        const bool ok = page.code >= 200 && page.code < 300 && !blocked;
        if (ok) markBrowserBound(url.host(), owner);
        else if (page.error.isEmpty())
            page.error = g_lastHumanCheck == HumanCheck::Closed
                             ? QStringLiteral("%1 wants you to confirm you're human, and the check was "
                                              "closed before it cleared. Try again to get it back.").arg(url.host())
                         : g_lastHumanCheck == HumanCheck::TimedOut
                             ? QStringLiteral("%1 wants you to confirm you're human, and the check "
                                              "wasn't done in time. Try again to get it back.").arg(url.host())
                         : g_lastWasRefusal
                             ? QStringLiteral("%1 is blocking this network outright - its firewall "
                                              "answers with a block page rather than a challenge, "
                                              "so there is nothing to solve. A different "
                                              "connection is what this needs.").arg(url.host())
                         : blocked
                             ? QStringLiteral("%1 still refuses the web engine after renewing its "
                                              "clearance").arg(url.host())
                         : page.code == 0
                             ? QStringLiteral("The web engine could not fetch %1").arg(url.host())
                             : QString();
        return page;
    }

    static bool settledOn(const QString &owner, const QUrl &root, const CancelToken &cancel,
                   const QDeadlineTimer &deadline) {
        auto match = std::make_shared<std::atomic_bool>(false);
        const bool answered = runOnGui([owner, root, match]() {
            const Session *entry = Host::instance().find(owner);
            match->store(entry && !entry->settledUrl.isEmpty()
                         && originRoot(entry->settledUrl) == root);
        }, deadline, cancel);
        return answered && match->load();
    }

    static QString sessionUserAgent(const QString &owner, const CancelToken &cancel,
                             const QDeadlineTimer &deadline) {
        auto agent = std::make_shared<QString>();
        const bool answered = runOnGui([owner, agent]() {
            if (const Session *entry = Host::instance().find(owner)) *agent = entry->userAgent;
        }, deadline, cancel);
        return answered ? *agent : QString();
    }

    static bool settle(const QUrl &document, const CancelToken &cancel, QDeadlineTimer &deadline,
                const QString &owner) {
        auto state = std::make_shared<SettleState>();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 budget = deadline.remainingTime();
        state->deadlineMs = now + budget;
        const qint64 reloadAtMs = now + budget * kReloadAtPercent / 100;
        const QString expectedOwner = providerSession(document.host());
        g_lastHumanCheck = HumanCheck::None;

        auto listeners = std::make_shared<QList<QMetaObject::Connection>>();
        const bool posted = runOnGui([owner, document, state, expectedOwner, reloadAtMs, listeners]() {
            Session &entry = Host::instance().session(owner);
            QWebEnginePage *page = entry.page;
            const quint64 navigation = ++entry.navigation;
            entry.settledUrl.clear();

            *listeners << QObject::connect(page, &QWebEnginePage::loadFinished, page,
                             [page, state, owner, expectedOwner, navigation, reloadAtMs](bool ok) {
                                 if (!ok) state->note(QStringLiteral("the navigation failed"));
                                 watchSettle(page, state, owner, expectedOwner, navigation, reloadAtMs);
                             }, Qt::SingleShotConnection);
            *listeners << QObject::connect(page, &QWebEnginePage::renderProcessTerminated, page,
                             [state](QWebEnginePage::RenderProcessTerminationStatus status,
                                     int exitCode) {
                                 state->note(QStringLiteral("the render process stopped "
                                                            "(status %1, exit code %2)")
                                                 .arg(int(status)).arg(exitCode));
                                 if (!g_engineRan.load() && !g_engineBroken.exchange(true))
                                     logWarn() << "Web" << QStringLiteral("Cloudflare:")
                                               << "the web engine cannot start here, so sites "
                                                  "behind Cloudflare will not load - status"
                                               << int(status) << "exit code" << exitCode;
                                 finishSettle(state);
                             }, Qt::SingleShotConnection);
            page->load(document);
        }, deadline, cancel);
        if (!posted) return false;

        while (!cancel.isCancelled() && QDateTime::currentMSecsSinceEpoch() <= state->deadlineMs.load())
            if (state->done.tryAcquire(1, 50)) break;
        g_lastWasRefusal = state->refused.load();
        runOnGui([listeners, state]() {
            for (const auto &listener : *listeners) QObject::disconnect(listener);
            finishSettle(state);
        }, QDeadlineTimer(3000), {});
        if (state->askedHuman) {
            g_lastHumanCheck = state->ok ? HumanCheck::Passed
                             : state->closedByUser ? HumanCheck::Closed : HumanCheck::TimedOut;
            runOnGui([owner]() { closeCheck(Host::instance().find(owner)); }, QDeadlineTimer(3000), {});
            if (state->ok) deadline = QDeadlineTimer(kSolveTimeoutMs);
        }
        if (state->ok) return true;
        if (cancel.isCancelled()) return false;
        if (g_lastHumanCheck == HumanCheck::Closed) {
            logWarn() << "Web" << QStringLiteral("Cloudflare:") << document.host() << "check closed before it cleared";
            return false;
        }
        if (g_lastWasRefusal)
            logWarn() << "Web" << QStringLiteral("Cloudflare:") << document.host()
                      << "served a block page to the web engine - that is a firewall rule on this "
                         "network, not a challenge, so retrying will not clear it -" << state->lastNote();
        else
            logWarn() << "Web" << QStringLiteral("Cloudflare:") << owner << "did not settle on" << document.host()
                      << "-" << state->lastNote();
        return false;
    }

    static PageFetch fetchOnce(const QUrl &url, const CancelToken &cancel, const QDeadlineTimer &deadline,
                        bool bytes, const QMap<QString, QString> &headers, const QString &owner) {
        PageFetch page;
        static QAtomicInteger<quint64> lastId;
        const quint64 id = lastId.fetchAndAddRelaxed(1) + 1;

        QJsonObject requestHeaders;
        QString referrer;
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
            const QString key = it.key().toLower();
            if (key == "referer" || key == "referrer") { referrer = it.value(); continue; }
            if (key == "cookie" || key == "user-agent" || key == "host" || key == "origin"
                || key == "content-length" || key == "connection" || key == "accept-encoding"
                || key.startsWith("sec-"))
                continue;
            requestHeaders.insert(it.key(), it.value());
        }
        QJsonObject options{{"credentials", "include"}, {"cache", "no-store"},
                            {"redirect", "follow"}, {"headers", requestHeaders}};
        if (!referrer.isEmpty()) options.insert("referrer", referrer);

        const QString script =
            QStringLiteral("(function(){var s=window.__aoFetch||(window.__aoFetch={});var id='")
            + QString::number(id) + QStringLiteral("';s[id]=null;(async function(){var out;")
            + QStringLiteral("var c=new AbortController();var t=setTimeout(function(){c.abort();},")
            + QString::number(qMax<qint64>(1, deadline.remainingTime()))
            + QStringLiteral(");try{var o=") + jsLiteral(options)
            + QStringLiteral(";o.signal=c.signal;var r=await fetch(") + jsLiteral(url.toString())
            + QStringLiteral(",o);var b=new Uint8Array(await r.arrayBuffer());if(b.length>")
            + QString::number(kMaxBrowserBodyBytes)
            + QStringLiteral(")throw new Error('response exceeds the browser transfer limit');"
                             "var body;if(")
            + (bytes ? QStringLiteral("true") : QStringLiteral("false"))
            + QStringLiteral("){var p='';for(var i=0;i<b.length;i+=32768)"
                             "p+=String.fromCharCode.apply(null,b.subarray(i,i+32768));"
                             "body=btoa(p);}else body=new TextDecoder().decode(b);"
                             "out=JSON.stringify({code:r.status,url:r.url,"
                             "headers:Object.fromEntries(r.headers.entries()),body:body});}"
                             "catch(e){out=JSON.stringify({code:0,error:((e&&e.name)||'Error')"
                             "+': '+((e&&e.message)||'')});}"
                             "finally{clearTimeout(t);s[id]=out;}})();})();true");

        auto state = std::make_shared<FetchState>();
        const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + deadline.remainingTime();
        const bool posted = runOnGui([owner, script, state, id, deadlineMs]() {
            Session &entry = Host::instance().session(owner);
            entry.page->runJavaScript(script);
            pollFetched(entry.page, state, id, deadlineMs);
        }, deadline, cancel);
        if (!posted) {
            page.error = QStringLiteral("The web engine request was cancelled");
            return page;
        }

        while (!deadline.hasExpired() && !cancel.isCancelled())
            if (state->done.tryAcquire(1, 50)) break;
        QString payload;
        { QMutexLocker lock(&state->mutex); payload = state->payload; }
        if (payload.isEmpty()) return page;

        const QJsonObject json = QJsonDocument::fromJson(payload.toUtf8()).object();
        page.code = json.value("code").toInt();
        page.error = json.value("error").toString();
        page.finalUrl = QUrl(json.value("url").toString());
        const QJsonObject responseHeaders = json.value("headers").toObject();
        for (auto it = responseHeaders.constBegin(); it != responseHeaders.constEnd(); ++it)
            page.headers[it.key()] = it.value().toString();
        page.contentType = page.headers.value(QStringLiteral("content-type"));
        if (bytes) page.bytes = QByteArray::fromBase64(json.value("body").toString().toLatin1());
        else       page.body = json.value("body").toString();
        return page;
    }
};

}

PageFetch Cloudflare::fetchInBrowser(const QUrl &url, const CancelToken &cancel, int timeoutMs, bool bytes,
                                     const QMap<QString, QString> &headers, const QString &sessionOwner) {
    const QString owner = ownerOf(url.host(), sessionOwner);
    if (!Browser::available()) {
        PageFetch page;
        page.error = QStringLiteral("The web engine is not available");
        return page;
    }
    return Browser::fetch(url, cancel, timeoutMs, bytes, headers, owner);
}

QString Cloudflare::solveChallenge(const QUrl &url, const CancelToken &cancel, int timeoutMs,
                                   const QString &sessionOwner) {
    if (cancel.isCancelled() || shutdownToken().isCancelled() || !Browser::available()) return {};
    const QString owner = ownerOf(url.host(), sessionOwner);
    return Browser::solve(url, cancel, timeoutMs, owner);
}

void Cloudflare::shutdown() {
    shutdownToken().cancel();
    runOnGui([]() { Host::instance().closeAll(); }, QDeadlineTimer(3000), {});
    CookieStore::instance().flush();
}

QString Cloudflare::captureInBrowser(const QUrl &url, const QString &hook, const QString &probe,
                                     const CancelToken &requestCancel, int timeoutMs,
                                     const QString &owner) {
    if (!Browser::available()) return {};
    const CancelToken cancel = requestCancel.composeWith(shutdownToken());
    const QDeadlineTimer deadline(qMax(1, timeoutMs));
    Lane lane(owner, cancel, deadline);
    if (!lane.acquire(Lane::Exclusive)) return {};

    static QAtomicInteger<quint64> lastRun;
    const QString run = QString::number(lastRun.fetchAndAddRelaxed(1) + 1);
    const QString marked = QStringLiteral("window.__aoCapture=") + run + QLatin1Char(';') + hook;
    const QString guarded = QStringLiteral("(window.__aoCapture===") + run
                          + QStringLiteral("?(") + probe + QStringLiteral("):'')");

    auto state = std::make_shared<FetchState>();
    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + deadline.remainingTime();
    const bool posted = runOnGui([owner, url, marked, guarded, state, deadlineMs]() {
        Session &entry = Host::instance().session(owner);
        ++entry.navigation;
        entry.settledUrl.clear();
        QWebEngineScriptCollection &scripts = entry.page->scripts();
        for (const QWebEngineScript &old : scripts.find(QStringLiteral("aowave-capture")))
            scripts.remove(old);
        QWebEngineScript script;
        script.setName(QStringLiteral("aowave-capture"));
        script.setSourceCode(marked);
        script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        script.setWorldId(QWebEngineScript::MainWorld);
        script.setRunsOnSubFrames(false);
        scripts.insert(script);
        entry.page->load(url);
        pollFor(entry.page, guarded, state, deadlineMs);
    }, deadline, cancel);
    if (!posted) return {};

    while (!deadline.hasExpired() && !cancel.isCancelled())
        if (state->done.tryAcquire(1, 50)) break;
    QMutexLocker lock(&state->mutex);
    return state->payload;
}
