#include "net/client.h"
#include "net/cloudflare.h"
#include "core/logger.h"
#include "net/dnsoverhttps.h"
#include "net/providerhealth.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QEventLoop>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QHash>

namespace {

// Hosts that answered 429, and when they said to come back. Shared by every client, so a batch of
// parallel requests waits out one Retry-After together instead of each spending more of the
// allowance (animepahe.pw allows about ten requests in ten seconds, then 429s for ten).
QMutex g_pauseMutex;
QHash<QString, qint64> g_resumeAtMs;

qint64 resumeAtMs(const QString &host) {
    QMutexLocker lock(&g_pauseMutex);
    return g_resumeAtMs.value(host);
}

void pauseHost(const QString &host, qint64 untilMs) {
    QMutexLocker lock(&g_pauseMutex);
    qint64 &resume = g_resumeAtMs[host];
    resume = qMax(resume, untilMs);
}

// Retry-After in seconds; the rare HTTP-date form, or none, gets Cloudflare's usual ten.
int retryAfterMs(const QMap<QString, QString> &headers) {
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare(QLatin1String("retry-after"), Qt::CaseInsensitive) != 0) continue;
        bool ok = false;
        const int seconds = it.value().trimmed().toInt(&ok);
        if (ok) return qBound(1, seconds, 30) * 1000;
    }
    return 10000;
}

}

QString Client::urlWithParams(const QString &url, const QMap<QString, QString> &params) {
    // Verbatim when there is nothing to add: a caller that built its own query - a signed one,
    // say - must get back the exact string it signed, not QUrl's normalisation of it.
    if (params.isEmpty()) return url;
    QUrl fullUrl(url);
    QUrlQuery query(fullUrl);
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        query.removeAllQueryItems(it.key());
        query.addQueryItem(it.key(), it.value());
    }
    fullUrl.setQuery(query);
    return fullUrl.toString(QUrl::FullyEncoded);
}

Client::Response Client::get(const QString &url, const QMap<QString, QString> &headers, const QMap<QString, QString> &params) {
    return request(GET, urlWithParams(url, params), headers, {});
}

Client::Response Client::getBytes(const QString &url, const QMap<QString, QString> &headers, const QMap<QString, QString> &params) {
    return request(GET, urlWithParams(url, params), headers, {}, true);
}

Client::Response Client::post(const QString &url, const QMap<QString, QString> &data, const QMap<QString, QString> &headers) {
    QUrlQuery query;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it)
        query.addQueryItem(it.key(), it.value());
    return request(POST, url, headers, query.query(QUrl::FullyEncoded).toUtf8());
}

static constexpr char k_defaultUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/139.0.0.0 Safari/537.36";

static const char *k_typeNames[] = {"GET", "POST", "HEAD"};

// Never deleted: tearing one down while the pool drains deadlocks on its network thread.
// Verified requests get managers of their own: a cached connection keeps ignoring the
// certificate errors a lenient request once ignored on it.
static QNetworkAccessManager *threadNetworkManager(const QString &owner, bool verified) {
    static thread_local QHash<std::pair<QString, bool>, QNetworkAccessManager *> managers;
    QNetworkAccessManager *&nam = managers[{owner, verified}];
    if (!nam) {
        nam = new QNetworkAccessManager;
        nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        nam->setCookieJar(new Cloudflare::ProxyCookieJar(nam, owner));
    }
    return nam;
}

// These repeat identically; timeouts and resets can come good.
static bool isDeadHost(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyConnectionRefusedError:
        return true;
    default:
        return false;
    }
}

struct Client::Raw {
    int code = -1;
    bool deadHost = false;
    bool hostNotFound = false;
    bool failed = false;
    QString error;
    QUrl finalUrl;
    QMap<QString, QString> headers;
    QByteArray body;
};

Client::Raw Client::fetchViaQt(int type, const QString &urlStr, const QMap<QString, QString> &headersMap,
                               const QByteArray &postData, const QString &address) {
    const QUrl parsedUrl(urlStr);
    const QString owner = m_session.isEmpty() ? Cloudflare::providerSession(parsedUrl.host()) : m_session;
    QNetworkRequest request{parsedUrl};
    request.setTransferTimeout(m_timeoutMs);
    if (!address.isEmpty()) DnsOverHttps::pin(request, address);

    // ok.ru signs urls to an exact UA.
    bool hasUserAgent = false, hasAccept = false, hasAuthorization = false;
    QString callerCookies;
    for (auto it = headersMap.constBegin(); it != headersMap.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
        const QString lower = it.key().toLower();
        if (lower == "user-agent")         hasUserAgent = true;
        else if (lower == "accept")        hasAccept = true;
        else if (lower == "cookie")        callerCookies = it.value();
        else if (lower == "authorization") hasAuthorization = true;
    }

    // Qt would replace a caller's own Cookie header outright, and an address-pinned request
    // would look the jar up by IP.
    if (!callerCookies.isEmpty() || !address.isEmpty()) {
        const QByteArray merged = Cloudflare::CookieStore::instance().cookieHeader(parsedUrl, callerCookies, owner);
        request.setRawHeader("Cookie", merged);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    }

    if (const QString hostUa = Cloudflare::hostUserAgent(parsedUrl.host(), owner); !hostUa.isEmpty()) {
        request.setRawHeader("User-Agent", hostUa.toUtf8());
        hasUserAgent = true;
    }

    if (!hasUserAgent)
        request.setRawHeader("User-Agent", k_defaultUserAgent);
    if (!hasAccept)
        request.setRawHeader("Accept", "*/*");

    // Some CDNs serve valid streams behind certs Qt distrusts; mpv plays them. Anything that
    // carries a credential - a token, a session cookie, a form with a secret - is never sent
    // over a connection that could not be verified.
    const bool carriesCredentials = type == POST || hasAuthorization || !callerCookies.isEmpty();
    QNetworkAccessManager &manager = *threadNetworkManager(owner, carriesCredentials);
    QNetworkReply *reply = nullptr;
    switch (type) {
    case GET:  reply = manager.get(request); break;
    case POST: reply = manager.post(request, postData); break;
    case HEAD: reply = manager.head(request); break;
    default:   return {};
    }
    if (!carriesCredentials)
        QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                         [reply](const QList<QSslError> &) { reply->ignoreSslErrors(); });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QByteArray capped;
    bool truncated = false;
    if (m_maxBodyBytes > 0) {
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [&, reply]() {
            capped += reply->read(qMax<qint64>(0, m_maxBodyBytes - capped.size()));
            if (capped.size() >= m_maxBodyBytes && !truncated) { truncated = true; reply->abort(); }
        });
    }

    QTimer cancelTimer;
    cancelTimer.setInterval(50);
    QObject::connect(&cancelTimer, &QTimer::timeout, reply, [reply, this]() {
        if (isCancelled()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &cancelTimer, &QTimer::stop);
    cancelTimer.start();
    if (!reply->isFinished()) loop.exec();

    Raw raw;
    raw.finalUrl = reply->url();
    if (!address.isEmpty() && raw.finalUrl.host() == address) raw.finalUrl.setHost(parsedUrl.host());
    if (isCancelled()) { reply->deleteLater(); return raw; }

    if (const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute); status.isValid())
        raw.code = status.toInt();

    // Qt calls 403/503 an error and drops the payload, which is what tells CF from the origin.
    for (const QByteArray &header : reply->rawHeaderList())
        raw.headers[QString::fromUtf8(header)] = QString::fromUtf8(reply->rawHeader(header));
    // A timed-out reply has closed its device.
    raw.body = std::move(capped);
    if (reply->isOpen() && !truncated) raw.body += reply->readAll();

    // A short body is the cap working, not a failure.
    const QNetworkReply::NetworkError error = truncated ? QNetworkReply::NoError : reply->error();
    raw.failed = error != QNetworkReply::NoError;
    raw.deadHost = isDeadHost(error);
    raw.hostNotFound = error == QNetworkReply::HostNotFoundError;
    raw.error = reply->errorString();
    reply->deleteLater();
    return raw;
}

Client::Raw Client::fetchInBrowser(const QString &urlStr, bool binary, const QMap<QString, QString> &headers) {
    const Cloudflare::PageFetch page =
        Cloudflare::fetchInBrowser(QUrl(urlStr), m_cancel, Cloudflare::kSolveTimeoutMs, binary,
                                   headers, m_session);
    Raw raw;
    if (page.code == 0) {
        raw.error = page.error.isEmpty() ? QStringLiteral("browser fetch failed") : page.error;
        raw.failed = true;
        return raw;
    }
    raw.code = page.code;
    raw.failed = page.code >= 400;
    raw.headers = page.headers;
    raw.finalUrl = page.finalUrl;
    raw.error = page.error;
    raw.body = binary ? page.bytes : page.body.toUtf8();
    return raw;
}

// A rate limit is waited out rather than failed: the host says when to come back, and every
// request to it waits until then. Never on the GUI thread, which must not stall.
Client::Response Client::request(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData, bool binary) {
    // Long enough for a burst of parallel requests to take turns through a few windows.
    constexpr qint64 kMaxWaitMs = 60000;
    const QString host = QUrl(urlStr).host();
    const bool mayWait = !QThread::isMainThread();
    const CancelToken &shutdown = Cloudflare::shutdownToken();
    QElapsedTimer waited;
    waited.start();
    for (;;) {
        while (mayWait && !isCancelled() && !shutdown.isCancelled()
               && QDateTime::currentMSecsSinceEpoch() < resumeAtMs(host))
            QThread::msleep(50);
        Response response = requestOnce(type, urlStr, headersMap, postData, binary);
        if (response.code != 429 || isCancelled()) return response;
        const int waitMs = retryAfterMs(response.headers);
        if (!mayWait || shutdown.isCancelled() || waited.elapsed() + waitMs > kMaxWaitMs) {
            response.error = m_lastError =
                QObject::tr("%1 is limiting how often it can be asked. Wait a few seconds and try again.").arg(host);
            return response;
        }
        pauseHost(host, QDateTime::currentMSecsSinceEpoch() + waitMs + 250);
        logWarn() << "Network" << host << "is rate limiting; trying again in" << waitMs / 1000 << "s";
    }
}

Client::Response Client::requestOnce(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData, bool binary) {
    if (urlStr.isEmpty() || isCancelled()) return {};

    const QUrl parsedUrl(urlStr);
    const QString host = parsedUrl.host();
    if (!parsedUrl.isValid() || host.isEmpty() || (parsedUrl.scheme() != "https" && parsedUrl.scheme() != "http")) {
        Response response;
        response.error = m_lastError = QObject::tr("Invalid HTTP URL");
        return response;
    }

    auto blocked = [this](const Raw &raw) {
        // Only scan once the headers say Cloudflare is in the path.
        return m_bypass && Cloudflare::servedByProtectedEdge(raw.headers)
            && Cloudflare::isBlocked(raw.code, raw.headers,
                                     QString::fromUtf8(raw.body.left(Cloudflare::kBodyScanBytes)));
    };
    QElapsedTimer clock;
    clock.start();
    auto logged = [&](Raw raw) {
        if (isCancelled()) return raw;
        ProviderHealth::record(m_session, raw.code >= 200 && raw.code < 400 && !raw.failed,
                               clock.elapsed());
        if (m_verbose) {
            const QString msg = QString("%1 (%2)").arg(k_typeNames[type]).arg(raw.code);
            const QString safeUrl = parsedUrl.toString(QUrl::RemoveUserInfo);
            if (raw.code >= 200 && raw.code < 300) logOk() << msg << safeUrl;
            else                                 logWarn() << msg << safeUrl;
            if (raw.failed && !raw.error.isEmpty()) logWarn() << "Network" << raw.error;
        }
        return raw;
    };

    // A host that only answered the browser last time is asked through it again: its
    // clearance is bound to that TLS fingerprint and Qt's own stack would be re-challenged.
    bool viaBrowser = m_bypass && type == GET && Cloudflare::browserBound(host, m_session);
    // A host the system resolver has already failed on goes straight to the address it needed.
    const QString pinned = DnsOverHttps::cachedAddress(host);
    Raw raw = logged(viaBrowser ? fetchInBrowser(urlStr, binary, headersMap)
                                : fetchViaQt(type, urlStr, headersMap, postData, pinned));
    if (isCancelled()) return {};

    if (!viaBrowser && raw.hostNotFound && QHostAddress(host).isNull()) {
        if (const QString address = DnsOverHttps::resolve(host, m_cancel); !address.isEmpty()) {
            raw = logged(fetchViaQt(type, urlStr, headersMap, postData, address));
            if (isCancelled()) return {};
        }
    }

    // code 0 means the browser could not be driven at all, not that the host refused. Without
    // this the whole session is stranded on a web engine that has stopped working.
    if (viaBrowser && raw.code == 0) {
        viaBrowser = false;
        raw = logged(fetchViaQt(type, urlStr, headersMap, postData));
        if (isCancelled()) return {};
    }

    if (!viaBrowser && blocked(raw)) {
        Cloudflare::markBlocked(host);
        // One attempt owns both the solve and the request: a different TLS fingerprint re-challenges.
        const QUrl challengeUrl = raw.finalUrl.isEmpty() ? parsedUrl : raw.finalUrl;
        if (type == GET) {
            raw = logged(fetchInBrowser(challengeUrl.toString(), binary, headersMap));
            if (isCancelled()) return {};
        } else if (!Cloudflare::solveChallenge(challengeUrl, m_cancel, Cloudflare::kSolveTimeoutMs,
                                               m_session).isEmpty()) {
            raw = logged(fetchViaQt(type, urlStr, headersMap, postData));
            if (isCancelled()) return {};
        }
        if (blocked(raw)) {
            raw.failed = true;
            // The solver knows whether it met a challenge it could not finish or a firewall
            // rule that was never going to yield; that is worth far more than this sentence.
            if (raw.error.isEmpty())
                raw.error = QObject::tr("%1 still refuses this provider session after the "
                                        "challenge was met").arg(host);
        }
    }

    Response response;
    response.code = raw.code;
    response.deadHost = raw.deadHost;
    response.error = raw.failed ? raw.error : QString();
    if (!response.error.isEmpty() || raw.code < 400)
        m_lastError = response.error;
    else if (raw.code >= 520 && raw.code <= 530)   // Cloudflare's "origin" family
        m_lastError = QObject::tr("%1 is down: its server is not answering (HTTP %2).").arg(host).arg(raw.code);
    else
        m_lastError = QObject::tr("%1 answered HTTP %2.").arg(host).arg(raw.code);
    response.finalUrl = raw.finalUrl;
    response.headers = raw.headers;
    // Keep the status and headers, but never parse a partial response as success.
    if (raw.failed) return response;
    if (binary) response.bytes = raw.body;
    else        response.body  = QString::fromUtf8(raw.body);
    return response;
}
