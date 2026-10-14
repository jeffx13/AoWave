#include "net/client.h"
#include "app/settings.h"
#include "net/cloudflare.h"
#include "app/logger.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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

// Never deleted: tearing one down while the pool drains deadlocks. Verified requests get their own: a cached connection keeps ignoring cert errors.
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

    // Qt would replace a caller's own Cookie header outright, and an address-pinned request would look the jar up by IP.
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

    // Lenient TLS is for CDN streams behind certs Qt distrusts (Settings > Network), never for a request carrying a credential
    // (iyf puts its session in the query of GET requests).
    static const QRegularExpression sessionInQuery(QStringLiteral("(?:^|&)(?:token|sign|autologin)=[^&]"));
    const bool verified = m_strictTls || !Settings::lenientTls() || type == POST || type == PUT || hasAuthorization
                          || !callerCookies.isEmpty() || sessionInQuery.match(parsedUrl.query()).hasMatch();
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

Client::Response Client::request(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData, bool binary) {
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
                QObject::tr("%1 is limiting requests. Try again in a few seconds.").arg(host);
            return response;
        }
        pauseHost(host, QDateTime::currentMSecsSinceEpoch() + waitMs + 250);
        logWarn() << "Web" << host << "is rate limiting; trying again in" << waitMs / 1000 << "s";
    }
}

Client::Response Client::requestOnce(int type, const QString &urlStr, const QMap<QString, QString> &headersMap, const QByteArray &postData, bool binary) {
    if (urlStr.isEmpty() || isCancelled()) return {};

    const QUrl parsedUrl(urlStr);
    const QString host = parsedUrl.host();
    if (!parsedUrl.isValid() || host.isEmpty() || (parsedUrl.scheme() != "https" && parsedUrl.scheme() != "http")) {
        Response response;
        response.error = m_lastError = QObject::tr("Invalid web address.");
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
            if (raw.failed && !raw.error.isEmpty()) logWarn() << "Web" << raw.error;
        }
        return raw;
    };

    bool viaBrowser = m_bypass && type == GET && Cloudflare::browserBound(host, m_session);
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
            if (raw.error.isEmpty())
                raw.error = QObject::tr("%1 still blocks access after the check passed.").arg(host);
        }
    }

    Response response;
    response.code = raw.code;
    response.deadHost = raw.deadHost;
    response.error = raw.failed ? raw.error : QString();
    if (!response.error.isEmpty() || raw.code < 400)
        m_lastError = response.error;
    else if (raw.code >= 520 && raw.code <= 530)
        m_lastError = QObject::tr("%1 is down (HTTP %2).").arg(host).arg(raw.code);
    else
        m_lastError = QObject::tr("%1 answered HTTP %2.").arg(host).arg(raw.code);
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

// On QML's image thread Client's wait loop ran the image loader's events and deleted the job being started, so the lookup runs on the pool.
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
    // SERVFAIL surfaces as a temporary failure, not HostNotFound, so any failure counts.
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
    // HTTP/2 sends the url's host (the address) as :authority, not the Host header, and hdslb answers 403.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
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

    // The disk cache answers first, stale or not: such a host refuses at start the covers it already gave.
    if (Cloudflare::behindCloudflare(host))
        image.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    if (const QString address = DnsOverHttps::addressIfSystemFails(host, Cloudflare::shutdownToken()); !address.isEmpty())
        DnsOverHttps::pin(image, address);
    return image;
}
