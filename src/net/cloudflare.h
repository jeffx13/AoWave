#pragma once
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QMap>
#include <QList>
#include <QUrl>
#include <QMutex>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QThreadPool>
#include <QHash>
#include <memory>
#include "net/canceltoken.h"

class QNetworkReply;
class QNetworkRequest;
class QObject;

namespace Cloudflare {

// Enough of a body to carry an interstitial's markers without decoding a whole page.
constexpr int kBodyScanBytes = 16384;

// A solve drives a real browser through a challenge: seconds, not milliseconds.
constexpr int kSolveTimeoutMs = 45000;

QString providerSession(const QString &host);

// Cancelled as teardown begins, so a worker waiting on the browser gives up instead of
// running to its deadline with no event loop left to serve it.
const CancelToken &shutdownToken();

class CookieStore {
public:
    static CookieStore &instance();

    QList<QNetworkCookie> cookiesForUrl(const QUrl &url, const QString &owner = {}) const;
    void setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url, const QString &owner = {});

    // Keeps each cookie's own domain: CF redirects before issuing the clearance.
    void insert(const QList<QNetworkCookie> &cookies, const QString &owner = {});
    void invalidateClearance(const QUrl &url, const QString &owner = {});

    // Caller's entries win unless theirs is an empty placeholder.
    QByteArray cookieHeader(const QUrl &url, const QString &existing = {}, const QString &owner = {}) const;

    void load();
    void save() const;
    void flush() const;   // Writes a save the rate limiter deferred.

private:
    CookieStore() = default;

    class Jar : public QNetworkCookieJar {
    public:
        using QNetworkCookieJar::allCookies;
        using QNetworkCookieJar::setAllCookies;
    };

    mutable QMutex m_mutex;
    mutable QMutex m_saveMutex;
    QHash<QString, std::shared_ptr<Jar>> m_jars;
    mutable qint64 m_lastSaveMs  = 0;
    mutable bool   m_pendingSave = false;
};

class ProxyCookieJar : public QNetworkCookieJar {
public:
    explicit ProxyCookieJar(QObject *parent = nullptr, const QString &owner = {})
        : QNetworkCookieJar(parent), m_owner(owner) {}
    QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const override;
    bool setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url) override;
private:
    QString m_owner;
};

// Lets a caller skip decoding a body that cannot hold a challenge.
bool servedByProtectedEdge(const QMap<QString, QString> &headers);
bool isBlocked(int code, const QMap<QString, QString> &headers, const QString &body);

// Marker scans over a rendered page's visible text, for the solver's in-page probe.
bool looksLikeChallengePage(const QString &text);
bool looksLikeBlockPage(const QString &text);

void markBlocked(const QString &host);

// Clearance bound to the solver's TLS fingerprint, so no other client can present it.
bool browserBound(const QString &host, const QString &owner = {});
void markBrowserBound(const QString &host, const QString &owner = {});

struct PageFetch {
    int code = 0;          // 0 when the browser could not be driven
    QString contentType;
    QString body;
    QByteArray bytes;
    QMap<QString, QString> headers;
    QUrl finalUrl;
    QString error;
};
// Worker thread only; one at a time per provider.
PageFetch fetchInBrowser(const QUrl &url, const CancelToken &cancel = {},
                         int timeoutMs = kSolveTimeoutMs, bool bytes = false,
                         const QMap<QString, QString> &headers = {}, const QString &owner = {});
// Loads `url` with `hook` run before the page's own scripts, then evaluates `probe` until it
// returns a non-empty string, which is returned. Empty if the web engine is unavailable or
// time runs out. Worker thread only.
QString captureInBrowser(const QUrl &url, const QString &hook, const QString &probe,
                         const CancelToken &cancel, int timeoutMs, const QString &owner);
// Its own pool, so a burst of posters cannot park every global pool thread on the browser.
QThreadPool &pageFetchPool();
QNetworkReply *createBrowserReply(const QNetworkRequest &request, QObject *parent);

QString hostUserAgent(const QString &host, const QString &owner = {});
// The browser's User-Agent, which the clearance it earned for `host` is bound to.
void rememberUserAgent(const QString &host, const QString &userAgent, const QString &owner);

void applyClearanceHeaders(const QUrl &url, QMap<QString, QString> &headers, const QString &owner = {});

// Clears the challenge in the browser for a request it cannot make itself (a POST), and returns
// the User-Agent the clearance is bound to; empty on failure. Worker thread only.
QString solveChallenge(const QUrl &url, const CancelToken &cancel = {},
                       int timeoutMs = kSolveTimeoutMs, const QString &owner = {});

// Closes the browser and saves the cookies. GUI thread, as the app quits.
void shutdown();

}
