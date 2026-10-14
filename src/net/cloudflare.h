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
#include <QObject>
#include <memory>
#include <qqmlintegration.h>
#include "app/async.h"

class QJSEngine;
class QNetworkReply;
class QNetworkRequest;
class QQmlEngine;

namespace Cloudflare {

constexpr int kBodyScanBytes = 16384;

constexpr int kSolveTimeoutMs = 45000;

QString providerSession(const QString &host);
inline QString ownerOf(const QString &host, const QString &owner) { return owner.isEmpty() ? providerSession(host) : owner; }

// A site Cloudflare fronts, or any host holding a clearance: its images are refused until one exists.
bool behindCloudflare(const QString &host);

const CancelToken &shutdownToken();

class CookieStore {
public:
    static CookieStore &instance();

    QList<QNetworkCookie> cookiesForUrl(const QUrl &url, const QString &owner = {}) const;
    void setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url, const QString &owner = {});

    void insert(const QList<QNetworkCookie> &cookies, const QString &owner = {});
    void invalidateClearance(const QUrl &url, const QString &owner = {});

    QByteArray cookieHeader(const QUrl &url, const QString &existing = {}, const QString &owner = {}) const;

    void load();
    void save() const;
    void flush() const;

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

bool servedByProtectedEdge(const QMap<QString, QString> &headers);
bool isBlocked(int code, const QMap<QString, QString> &headers, const QString &body);

bool looksLikeChallengePage(const QString &text);
bool looksLikeBlockPage(const QString &text);

void markBlocked(const QString &host);

bool browserBound(const QString &host, const QString &owner = {});
void markBrowserBound(const QString &host, const QString &owner = {});

struct PageFetch {
    int code = 0;
    QString contentType;
    QString body;
    QByteArray bytes;
    QMap<QString, QString> headers;
    QUrl finalUrl;
    QString error;
};
PageFetch fetchInBrowser(const QUrl &url, const CancelToken &cancel = {},
                         int timeoutMs = kSolveTimeoutMs, bool bytes = false,
                         const QMap<QString, QString> &headers = {}, const QString &owner = {});
QString captureInBrowser(const QUrl &url, const QString &hook, const QString &probe,
                         const CancelToken &cancel, int timeoutMs, const QString &owner);
// Its own pool, so a burst of posters cannot park every global pool thread on the browser.
QThreadPool &pageFetchPool();
QNetworkReply *createBrowserReply(const QNetworkRequest &request, QObject *parent);

QString hostUserAgent(const QString &host, const QString &owner = {});
void rememberUserAgent(const QString &host, const QString &userAgent, const QString &owner);

void applyClearanceHeaders(const QUrl &url, QMap<QString, QString> &headers, const QString &owner = {});

QString solveChallenge(const QUrl &url, const CancelToken &cancel = {},
                       int timeoutMs = kSolveTimeoutMs, const QString &owner = {});

// Counts the clearances the web engine has earned, for QML: a cover refused before one is asked for again.
// Its own singleton rather than a member of App, which the render tests never create.
class Clearances : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    static Clearances &instance();
    static Clearances *create(QQmlEngine *, QJSEngine *);

    int count() const { return m_count; }
    void earned() { ++m_count; emit countChanged(); }

signals:
    void countChanged();

private:
    int m_count = 0;
};

// Closes the browser and saves the cookies. GUI thread, as the app quits.
void shutdown();

}
