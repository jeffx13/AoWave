#include "net/client.h"
#include "app/settings.h"
#include "net/cloudflare.h"
#include "app/logger.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>
#include <QRegularExpression>
#include <QSslError>
#include <QEventLoop>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QHash>
#include <zlib.h>
#include <QList>
#include <algorithm>
#include <QHostInfo>
#include <QSet>
#include <QThreadPool>
#include <future>
#include <memory>

namespace {

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

int retryAfterMs(const QMap<QString, QString> &headers) {
    bool ok = false;
    const int seconds = Client::headerValue(headers, "retry-after").trimmed().toInt(&ok);
    return ok ? qBound(1, seconds, 30) * 1000 : 10000;
}

QByteArray formEncoded(const QMap<QString, QString> &data) {
    QByteArrayList pairs;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it)
        pairs << QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
    return pairs.join('&');
}

QMap<QString, QString> withFormType(QMap<QString, QString> headers) {
    if (!Client::headerValue(headers, "content-type").isEmpty()) return headers;
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
    return headers;
}

}

QString Client::urlWithParams(const QString &url, const QMap<QString, QString> &params) {
    if (params.isEmpty()) return url;
    QUrl fullUrl(url);
    QUrlQuery query(fullUrl);
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) query.removeAllQueryItems(it.key());
    QByteArray encoded = query.query(QUrl::FullyEncoded).toUtf8();
    if (!encoded.isEmpty()) encoded += '&';
    fullUrl.setQuery(QString::fromLatin1(encoded + formEncoded(params)));
    return fullUrl.toString(QUrl::FullyEncoded);
}

Client::Response Client::get(const QString &url, const QMap<QString, QString> &headers, const QMap<QString, QString> &params) {
    return request(GET, urlWithParams(url, params), headers, {});
}

Client::Response Client::getBytes(const QString &url, const QMap<QString, QString> &headers, const QMap<QString, QString> &params) {
    return request(GET, urlWithParams(url, params), headers, {}, true);
}

Client::Response Client::post(const QString &url, const QMap<QString, QString> &data, const QMap<QString, QString> &headers) {
    return request(POST, url, withFormType(headers), formEncoded(data));
}

Client::Response Client::put(const QString &url, const QMap<QString, QString> &data, const QMap<QString, QString> &headers) {
    return request(PUT, url, withFormType(headers), formEncoded(data));
}

static const char *k_typeNames[] = {"GET", "POST", "HEAD", "PUT"};

// never deleted: teardown deadlocks the draining pool
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

QString Client::networkMessage(int error, const QString &host) {
    switch (QNetworkReply::NetworkError(error)) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QObject::tr("%1 did not respond. Try again later.").arg(host);
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::UnknownNetworkError:
        return QObject::tr("Could not reach %1. Check your connection.").arg(host);
    case QNetworkReply::RemoteHostClosedError:
        return QObject::tr("%1 closed the connection. Try again later.").arg(host);
    case QNetworkReply::SslHandshakeFailedError:
        return QObject::tr("Could not open a secure connection to %1.").arg(host);
    default:
        return QObject::tr("Could not load from %1. Try again later.").arg(host);
    }
}

QString Client::httpMessage(int code, const QString &host) {
    if (code == 429) return QObject::tr("%1 is limiting requests. Try again in a few seconds.").arg(host);
    if (code == 401 || code == 403) return QObject::tr("%1 refused the request (HTTP %2). Try another provider.").arg(host).arg(code);
    if (code == 404 || code == 410) return QObject::tr("%1 could not find that (HTTP %2).").arg(host).arg(code);
    if (code >= 520 && code <= 530) return QObject::tr("%1 is down (HTTP %2).").arg(host).arg(code);
    if (code >= 500) return QObject::tr("%1 had a problem (HTTP %2). Try again later.").arg(host).arg(code);
    return QObject::tr("%1 answered HTTP %2.").arg(host).arg(code);
}

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
    int netError = 0;
    QString error;
    QString detail;
    QUrl finalUrl;
    QMap<QString, QString> headers;
    QByteArray body;
};

Client::Raw Client::fetchViaQt(int type, const QString &urlStr, const QMap<QString, QString> &headersMap,
                               const QByteArray &postData, const QString &address, int redirects) {
    const QUrl parsedUrl(urlStr);
    const QString owner = Cloudflare::ownerOf(parsedUrl.host(), m_session);
    QNetworkRequest request{parsedUrl};
    request.setTransferTimeout(m_timeoutMs);
    if (!address.isEmpty()) DnsOverHttps::pin(request, address);

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
        request.setRawHeader("User-Agent", kChromeUserAgent);
    if (!hasAccept)
        request.setRawHeader("Accept", "*/*");

    // no lenient TLS with a credential (iyf signs in the query)
    static const QRegularExpression sessionInQuery(QStringLiteral("(?:^|&)(?:token|sign|autologin)=[^&]"));
    const bool verified = m_strictTls || !Settings::lenientTls() || type == POST || type == PUT || hasAuthorization
                          || !callerCookies.isEmpty() || sessionInQuery.match(parsedUrl.query()).hasMatch()
                          || !Cloudflare::CookieStore::instance().cookiesForUrl(parsedUrl, owner).isEmpty();
    if (!verified) {
        static QMutex lenientMutex;
        static QSet<QString> lenientHosts;
        QMutexLocker lock(&lenientMutex);
        if (!lenientHosts.contains(parsedUrl.host())) {
            lenientHosts.insert(parsedUrl.host());
            logInfo() << "Web" << parsedUrl.host() << "lenient TLS";
        }
    }
    // Qt carries ignored cert errors over to the redirect
    if (!verified)
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkAccessManager &manager = *threadNetworkManager(owner, verified);
    QNetworkReply *reply = nullptr;
    switch (type) {
    case GET:  reply = manager.get(request); break;
    case POST: reply = manager.post(request, postData); break;
    case HEAD: reply = manager.head(request); break;
    case PUT:  reply = manager.put(request, postData); break;
    default:   return {};
    }
    if (!verified)
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

    int streamCode = 0;
    bool sinkClosed = false;
    QMap<QString, QString> streamHeaders;
    const auto feedSink = [&, reply]() {
        if (sinkClosed) return;
        if (!streamCode) {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code < 200 || code >= 300) return;
            streamCode = code;
            for (const QByteArray &header : reply->rawHeaderList())
                streamHeaders[QString::fromUtf8(header)] = QString::fromUtf8(reply->rawHeader(header));
        }
        if (const QByteArray chunk = reply->readAll(); !chunk.isEmpty() && !m_sink(streamCode, streamHeaders, chunk)) {
            sinkClosed = true;
            reply->abort();
        }
    };
    if (m_sink) QObject::connect(reply, &QNetworkReply::readyRead, reply, feedSink);

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

    if (const QVariant target = !verified ? reply->attribute(QNetworkRequest::RedirectionTargetAttribute) : QVariant();
        target.isValid()) {
        const QUrl next = reply->url().resolved(target.toUrl());
        reply->deleteLater();
        constexpr int kMaxRedirects = 5;
        const bool web = next.scheme() == QLatin1String("https") || next.scheme() == QLatin1String("http");
        const bool downgrade = parsedUrl.scheme() == QLatin1String("https") && next.scheme() != QLatin1String("https");
        if (redirects < kMaxRedirects && web && !downgrade)
            return fetchViaQt(type, next.toString(QUrl::FullyEncoded), headersMap, postData, {}, redirects + 1);
        raw.failed = true;
        raw.error = redirects >= kMaxRedirects ? QStringLiteral("Too many redirects")
                                               : QStringLiteral("Redirect to %1 refused").arg(next.scheme());
        return raw;
    }

    if (const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute); status.isValid())
        raw.code = status.toInt();

    for (const QByteArray &header : reply->rawHeaderList())
        raw.headers[QString::fromUtf8(header)] = QString::fromUtf8(reply->rawHeader(header));
    raw.body = std::move(capped);
    if (m_sink && reply->isOpen()) feedSink();
    if (reply->isOpen() && !truncated && !streamCode) raw.body += reply->readAll();

    const QNetworkReply::NetworkError error = truncated ? QNetworkReply::NoError : reply->error();
    raw.failed = error != QNetworkReply::NoError;
    raw.deadHost = isDeadHost(error);
    raw.hostNotFound = error == QNetworkReply::HostNotFoundError;
    raw.netError = int(error);
    raw.detail = reply->errorString();
    if (raw.failed)
        raw.error = raw.code >= 400 ? httpMessage(raw.code, parsedUrl.host()) : networkMessage(int(error), parsedUrl.host());
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

Client::Response Client::request(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData, bool binary) {
    constexpr qint64 kMaxWaitMs = 60000;
    const QString host = QUrl(urlStr).host();
    const bool mayWait = !QThread::isMainThread();
    const CancelToken &shutdown = Cloudflare::shutdownToken();
    constexpr int kRetryDelayMs = 600;
    QElapsedTimer waited;
    waited.start();
    bool retried = false;
    for (;;) {
        while (mayWait && !isCancelled() && !shutdown.isCancelled()
               && QDateTime::currentMSecsSinceEpoch() < resumeAtMs(host))
            QThread::msleep(50);
        bool transient = false;
        Response response = requestOnce(type, urlStr, headersMap, postData, binary, &transient);
        if (transient && !retried && mayWait && !isCancelled() && !shutdown.isCancelled()) {
            retried = true;
            logWarn() << "Web" << host << QStringLiteral("failed (%1); trying once more")
                                              .arg(response.code > 0 ? QStringLiteral("HTTP %1").arg(response.code) : response.error);
            for (int slept = 0; slept < kRetryDelayMs && !isCancelled() && !shutdown.isCancelled(); slept += 50)
                QThread::msleep(50);
            continue;
        }
        if (response.code != 429 || isCancelled()) return response;
        const int waitMs = retryAfterMs(response.headers);
        if (!mayWait || shutdown.isCancelled() || waited.elapsed() + waitMs > kMaxWaitMs) {
            response.error = setLastError(httpMessage(429, host), 429);
            return response;
        }
        pauseHost(host, QDateTime::currentMSecsSinceEpoch() + waitMs + 250);
        logWarn() << "Web" << host << "is rate limiting; trying again in" << waitMs / 1000 << "s";
    }
}

Client::Response Client::requestOnce(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData,
                                     bool binary, bool *transient) {
    if (urlStr.isEmpty() || isCancelled()) return {};

    const QUrl parsedUrl(urlStr);
    const QString host = parsedUrl.host();
    if (!parsedUrl.isValid() || host.isEmpty() || (parsedUrl.scheme() != "https" && parsedUrl.scheme() != "http")) {
        Response response;
        response.error = setLastError(QObject::tr("Invalid web address."));
        return response;
    }

    auto blocked = [this](const Raw &raw) {
        return m_bypass && Cloudflare::servedByProtectedEdge(raw.headers)
            && Cloudflare::isBlocked(raw.code, raw.headers,
                                     QString::fromUtf8(raw.body.left(Cloudflare::kBodyScanBytes)));
    };
    QElapsedTimer clock;
    clock.start();
    auto logged = [&](Raw raw) {
        if (isCancelled()) return raw;
        ProviderHealth::record(m_provider, raw.code >= 200 && raw.code < 400 && !raw.failed,
                               clock.elapsed());
        if (m_verbose) {
            const QString msg = QString("%1 (%2)").arg(k_typeNames[type]).arg(raw.code);
            const QString safeUrl = parsedUrl.toString(QUrl::RemoveUserInfo);
            if (raw.code >= 200 && raw.code < 300) logOk() << msg << safeUrl;
            else                                 logWarn() << msg << safeUrl;
            if (raw.failed && !raw.error.isEmpty()) logWarn() << "Web" << (raw.detail.isEmpty() ? raw.error : raw.detail);
        }
        return raw;
    };

    bool viaBrowser = m_bypass && type == GET && Cloudflare::browserBound(host, m_session);
    bool usedBrowser = viaBrowser;
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

    if (viaBrowser && raw.code == 0) {
        viaBrowser = false;
        raw = logged(fetchViaQt(type, urlStr, headersMap, postData));
        if (isCancelled()) return {};
    }

    if (!viaBrowser && blocked(raw)) {
        Cloudflare::markBlocked(host);
        // solve and request in one go: a new TLS fingerprint re-challenges
        const QUrl challengeUrl = raw.finalUrl.isEmpty() ? parsedUrl : raw.finalUrl;
        if (type == GET) {
            usedBrowser = true;
            raw = logged(fetchInBrowser(challengeUrl.toString(), binary, headersMap));
            if (isCancelled()) return {};
        } else if (!Cloudflare::solveChallenge(challengeUrl, m_cancel, Cloudflare::kSolveTimeoutMs,
                                               m_session).isEmpty()) {
            raw = logged(fetchViaQt(type, urlStr, headersMap, postData));
            if (isCancelled()) return {};
        }
        if (blocked(raw)) {
            raw.failed = true;
            if (raw.error.isEmpty())
                raw.error = QObject::tr("%1 still blocks access after the check passed.").arg(host);
        }
    }

    if (transient)
        *transient = type == GET && raw.failed && !m_sink && !usedBrowser && !Cloudflare::servedByProtectedEdge(raw.headers)
                     && (raw.netError == QNetworkReply::TimeoutError || raw.netError == QNetworkReply::RemoteHostClosedError
                         || raw.code == 502 || raw.code == 503 || raw.code == 504);

    Response response;
    response.code = raw.code;
    response.deadHost = raw.deadHost;
    response.error = raw.failed ? raw.error : QString();
    setLastError(!response.error.isEmpty() || raw.code < 400 ? response.error : httpMessage(raw.code, host), raw.code);
    response.finalUrl = raw.finalUrl;
    response.headers = raw.headers;
    if (raw.failed) {
        if (!binary) response.errorBody = QString::fromUtf8(raw.body.left(8192));
        return response;
    }
    if (binary) response.bytes = raw.body;
    else        response.body  = QString::fromUtf8(raw.body);
    return response;
}

namespace {
QByteArray run(const QByteArray &input, int windowBits) {
    if (input.isEmpty()) return {};
    z_stream stream{};
    if (inflateInit2(&stream, windowBits) != Z_OK) return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = uInt(input.size());

    QByteArray out;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = uInt(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) break;
        out.append(chunk.constData(), chunk.size() - qsizetype(stream.avail_out));
        if (out.size() > 32 * 1024 * 1024) { status = Z_DATA_ERROR; break; }
    } while (status != Z_STREAM_END && stream.avail_in > 0);
    inflateEnd(&stream);
    return status == Z_STREAM_END ? out : QByteArray();
}
}

QByteArray Decompress::gzipOrZlib(const QByteArray &input) { return run(input, MAX_WBITS + 32); }

QByteArray Decompress::rawDeflate(const QByteArray &input) { return run(input, -MAX_WBITS); }

namespace {

constexpr int kWindow = 60;

struct Record {
    QList<bool>   outcomes;
    QList<qint64> latencies;
};

QMutex g_mutex;
QHash<QString, Record> g_records;

template <typename T>
void push(QList<T> &list, T value) {
    list.append(value);
    if (list.size() > kWindow) list.removeFirst();
}

}

void ProviderHealth::record(const QString &owner, bool ok, qint64 elapsedMs) {
    if (owner.isEmpty()) return;
    QMutexLocker lock(&g_mutex);
    Record &entry = g_records[owner];
    push(entry.outcomes, ok);
    if (ok && elapsedMs >= 0) push(entry.latencies, elapsedMs);
}

QVariantMap ProviderHealth::statsFor(const QString &owner) {
    QMutexLocker lock(&g_mutex);
    const auto it = g_records.constFind(owner);
    if (it == g_records.constEnd() || it->outcomes.isEmpty()) return {};

    const int total = int(it->outcomes.size());
    int failures = 0;
    for (bool ok : it->outcomes) if (!ok) ++failures;

    qint64 median = 0;
    if (!it->latencies.isEmpty()) {
        QList<qint64> sorted = it->latencies;
        std::sort(sorted.begin(), sorted.end());
        median = sorted.at(sorted.size() / 2);
    }

    const double rate = double(total - failures) / total;
    return {
        {QStringLiteral("name"), owner},
        {QStringLiteral("requests"), total},
        {QStringLiteral("failures"), failures},
        {QStringLiteral("successRate"), rate},
        {QStringLiteral("medianMs"), median},
        {QStringLiteral("healthy"), rate >= 0.67},
    };
}

void ProviderHealth::reset(const QString &owner) {
    QMutexLocker lock(&g_mutex);
    if (owner.isEmpty()) g_records.clear();
    else                 g_records.remove(owner);
}

namespace {

struct Entry {
    QString address;
    qint64  expiresAt = 0;
};

QMutex g_dnsMutex;
QHash<QString, Entry> g_overHttps;
QHash<QString, Entry> g_systemChecks;
QSet<QString> g_filteredDomains;

QString domainOf(const QString &host) {
    return host.section(QLatin1Char('.'), -2);
}

bool lookup(const QHash<QString, Entry> &cache, const QString &host, QString &address) {
    QMutexLocker lock(&g_dnsMutex);
    const auto it = cache.constFind(host);
    if (it == cache.constEnd() || it->expiresAt <= QDateTime::currentMSecsSinceEpoch()) return false;
    address = it->address;
    return true;
}

void remember(QHash<QString, Entry> &cache, const QString &host, const QString &address, int minutes) {
    QMutexLocker lock(&g_dnsMutex);
    cache.insert(host, {address, QDateTime::currentMSecsSinceEpoch() + minutes * 60 * 1000});
}

}

QString DnsOverHttps::resolve(const QString &host, const CancelToken &cancel) {
    QString address;
    if (lookup(g_overHttps, host, address)) return address;

    Client resolver(cancel, false);
    resolver.setBypassEnabled(false).setStrictTls(true);
    for (const char *endpoint : {"https://1.1.1.1/dns-query", "https://8.8.8.8/resolve"}) {
        const auto answer = resolver.get(QLatin1String(endpoint), {{"Accept", "application/dns-json"}},
                                         {{"name", host}, {"type", "A"}});
        for (const QJsonValue &record : answer.toJsonObject().value("Answer").toArray())
            if (record.toObject().value("type").toInt() == 1) {
                address = record.toObject().value("data").toString();
                break;
            }
        if (!address.isEmpty() || resolver.isCancelled()) break;
    }
    if (resolver.isCancelled()) return {};
    if (!address.isEmpty()) {
        logStep() << "Web" << host << "does not resolve here; reaching it at" << address
                  << "from DNS over HTTPS";
        QMutexLocker lock(&g_dnsMutex);
        g_filteredDomains.insert(domainOf(host));
    }
    remember(g_overHttps, host, address, address.isEmpty() ? 1 : 10);
    return address;
}

QString DnsOverHttps::cachedAddress(const QString &host) {
    QString address;
    return lookup(g_overHttps, host, address) ? address : QString();
}

namespace {

QThreadPool &lookupPool() {
    static QThreadPool pool;
    static const bool ready = [] {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(-1);
        return true;
    }();
    Q_UNUSED(ready);
    return pool;
}

// not on QML's image thread: its wait loop deleted the job
QString resolveElsewhere(const QString &host, const CancelToken &cancel) {
    auto task = std::make_shared<std::packaged_task<QString()>>(
        [host, cancel] { return DnsOverHttps::resolve(host, cancel); });
    std::future<QString> address = task->get_future();
    lookupPool().start([task] { (*task)(); });
    return address.get();
}

}

void DnsOverHttps::waitForLookups() {
    lookupPool().waitForDone();
}

QString DnsOverHttps::addressIfSystemFails(const QString &host, const CancelToken &cancel) {
    if (host.isEmpty() || !QHostAddress(host).isNull()) return {};
    QString address;
    if (lookup(g_systemChecks, host, address)) return address;
    bool filtered = false;
    {
        QMutexLocker lock(&g_dnsMutex);
        filtered = g_filteredDomains.contains(domainOf(host));
    }
    if (filtered) return resolveElsewhere(host, cancel);
    const QHostInfo system = QHostInfo::fromName(host);
    // SERVFAIL is not HostNotFound
    const bool failed = system.error() != QHostInfo::NoError || system.addresses().isEmpty();
    address = failed ? resolveElsewhere(host, cancel) : QString();
    remember(g_systemChecks, host, address, 10);
    return address;
}

void DnsOverHttps::pin(QNetworkRequest &request, const QString &address) {
    QUrl target = request.url();
    const QString host = target.host();
    const QByteArray authority = target.authority(QUrl::FullyEncoded).toUtf8();
    target.setHost(address);
    request.setUrl(target);
    request.setPeerVerifyName(host);
    request.setRawHeader("Host", authority);
    // HTTP/2 sends the address as :authority; hdslb 403s
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
}

void Client::sleep(int ms) const {
    QElapsedTimer clock;
    clock.start();
    while (!isCancelled() && clock.elapsed() < ms) QThread::msleep(uint(std::min<qint64>(50, ms - clock.elapsed())));
}

QNetworkRequest imageRequest(const QNetworkRequest &request) {
    QNetworkRequest image(request);
    const QString host = request.url().host();
    if (host.endsWith("youtube-anime.com") && !request.hasRawHeader("Referer"))
        image.setRawHeader("Referer", "https://youtu-chan.com/");

    if (const QString clearedAgent = Cloudflare::hostUserAgent(host); !clearedAgent.isEmpty())
        image.setRawHeader("User-Agent", clearedAgent.toUtf8());
    else if (!request.hasRawHeader("User-Agent"))
        image.setRawHeader("User-Agent", kChromeUserAgent);
    if (!request.hasRawHeader("Accept"))
        image.setRawHeader("Accept", "*/*");

    if (Cloudflare::behindCloudflare(host))
        image.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    if (const QString address = DnsOverHttps::addressIfSystemFails(host, Cloudflare::shutdownToken()); !address.isEmpty())
        DnsOverHttps::pin(image, address);
    return image;
}

namespace {

class ImageAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &req, QIODevice *outgoingData) override {
        if (op == GetOperation && Cloudflare::browserBound(req.url().host()))
            return Cloudflare::createBrowserReply(req, this);
        return QNetworkAccessManager::createRequest(op, imageRequest(req), outgoingData);
    }
};

class ImageAccessManagerFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override {
        auto *manager = new ImageAccessManager(parent);
        manager->setCookieJar(new Cloudflare::ProxyCookieJar(manager));

        const QString cacheDir = Settings::tempDir() + QStringLiteral("/images");
        QDir().mkpath(cacheDir);
        auto *diskCache = new QNetworkDiskCache(manager);
        diskCache->setCacheDirectory(cacheDir);
        diskCache->setMaximumCacheSize(100LL * 1024 * 1024);
        manager->setCache(diskCache);
        return manager;
    }
};

}

QQmlNetworkAccessManagerFactory *imageNetworkFactory() {
    static ImageAccessManagerFactory factory;
    return &factory;
}
