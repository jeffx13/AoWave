#include "net/hlsproxy.h"
#include "system/platform.h"
#include "net/cloudflare.h"
#include "net/client.h"
#include <QCoreApplication>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QHash>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>

HlsProxy *HlsProxy::s_instance = nullptr;

static const QByteArray kUserAgent = kFirefoxUserAgent;

HlsProxy::HlsProxy(QObject *parent) : QTcpServer(parent) {
    s_instance = this;
    m_secret.resize(32);
    auto *words = reinterpret_cast<quint32 *>(m_secret.data());
    QRandomGenerator::system()->generate(words, words + 8);
    m_pool.setMaxThreadCount(16);
    m_pool.setExpiryTimeout(30000);
    ensureListening();

    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        close();
        m_cancel.cancel();
    });
}

HlsProxy::~HlsProxy() {
    close();
    m_cancel.cancel();
    m_pool.waitForDone();
    s_instance = nullptr;
}

void HlsProxy::ensureListening() {
    if (m_port.load() != 0) return;
    if (listen(QHostAddress::LocalHost, 0))
        m_port.store(serverPort());
}

static QString pct(const QString &s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }

// Anything on loopback reaches this port, so only serve urls the proxy minted.
static QByteArray sign(const QByteArray &secret, const QString &url, const QString &ref) {
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, secret);
    mac.addData(url.toUtf8());
    mac.addData("\x1f");
    mac.addData(ref.toUtf8());
    return mac.result().left(16).toHex();
}

static bool sameSignature(const QByteArray &expected, const QByteArray &given) {
    if (expected.size() != given.size()) return false;
    char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) diff |= char(expected[i] ^ given[i]);
    return diff == 0;
}

static QString proxyUrl(quint16 port, const QByteArray &secret, const char *endpoint,
                        const QString &url, const QString &ref) {
    return QString("http://127.0.0.1:%1/%2?u=%3&r=%4&s=%5")
        .arg(port).arg(QLatin1String(endpoint), pct(url), pct(ref),
                       QString::fromLatin1(sign(secret, url, ref)));
}

QString HlsProxy::playlistUrl(const QString &m3u8Url, const QString &referer) {
    quint16 p = m_port.load();
    if (p == 0) return m3u8Url;
    return proxyUrl(p, m_secret, "p.m3u8", m3u8Url, referer);
}

static QString rewritePlaylist(const QString &content, const QString &base, const QString &ref,
                               quint16 port, const QByteArray &secret) {
    static const QRegularExpression uriRe(QStringLiteral(R"RX(URI="([^"]+)")RX"));
    const QUrl baseUrl(base);
    QStringList out;
    for (const QString &line : content.split('\n')) {
        QString t = line.trimmed();
        if (t.isEmpty()) continue;
        if (t.startsWith('#')) {
            // Keys and init segments hide in attributes and 403 without our Referer.
            if (t.startsWith("#EXT-X-KEY") || t.startsWith("#EXT-X-SESSION-KEY") || t.startsWith("#EXT-X-MAP")) {
                const auto m = uriRe.match(t);
                if (m.hasMatch())
                    t.replace(m.capturedStart(1), m.capturedLength(1),
                              proxyUrl(port, secret, "k.bin",
                                       baseUrl.resolved(QUrl(m.captured(1))).toString(), ref));
            }
            out << t;
            continue;
        }
        const QString abs = baseUrl.resolved(QUrl(t)).toString();
        const QString path = abs.split('?').first();
        const char *endpoint = path.endsWith(".m3u8")                            ? "p.m3u8"
                             : (path.endsWith(".m4s") || path.endsWith(".mp4"))  ? "k.bin"
                                                                                 : "s.ts";
        out << proxyUrl(port, secret, endpoint, abs, ref);
    }
    return out.join('\n');
}

static int tsOffset(const QByteArray &d) {
    const int lim = std::min<int>(int(d.size()) - 188 * 3, 8192);
    for (int o = 0; o < lim; ++o) {
        if (quint8(d[o]) == 0x47 && quint8(d[o + 188]) == 0x47 &&
            quint8(d[o + 376]) == 0x47 && quint8(d[o + 564]) == 0x47)
            return o;
    }
    return 0;
}

struct Fetched {
    int code = 0;
    QByteArray data;
    QByteArray contentRange;
    bool failed = false;
    qint64 expected = -1;
};

static bool cutOff(const Fetched &f, qint64 received) {
    if (!f.failed) return false;
    return f.expected >= 0 ? received < f.expected : true;
}

static QMutex g_goneMutex;
static QHash<QString, qint64> g_gone;

static bool isGone(const QString &url) {
    QMutexLocker lock(&g_goneMutex);
    const auto it = g_gone.constFind(url);
    return it != g_gone.cend() && QDateTime::currentMSecsSinceEpoch() < it.value();
}

static void markGone(const QString &url) {
    QMutexLocker lock(&g_goneMutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    g_gone.removeIf([now](const auto &entry) { return entry.value() < now; });
    g_gone.insert(url, now + 60000);
}

static Fetched fetch(const QString &url, const QString &referer,
                     const QByteArray &range = {}, int timeoutMs = 0,
                     const CancelToken &cancel = {}, Client::Sink sink = {}) {
    QMap<QString, QString> headers{{QStringLiteral("User-Agent"), QString::fromLatin1(kUserAgent)}};
    if (!referer.isEmpty()) headers[QStringLiteral("Referer")] = referer;
    if (!range.isEmpty())   headers[QStringLiteral("Range")] = QString::fromLatin1(range);
    Cloudflare::applyClearanceHeaders(QUrl(url), headers);

    Client client(cancel, false);
    if (timeoutMs > 0) client.setTimeout(timeoutMs);
    if (sink) client.setSink(std::move(sink));
    const Client::Response response = client.getBytes(url, headers);

    Fetched f;
    f.code = response.code;
    f.data = response.bytes;
    f.contentRange = response.header("Content-Range").toLatin1();
    f.failed = !response.error.isEmpty();
    bool known = false;
    f.expected = response.header("Content-Length").toLongLong(&known);
    if (!known) f.expected = -1;
    return f;
}

static Fetched fetchPlaylist(const QString &url, const QString &referer, const CancelToken &cancel) {
    constexpr int kBuildTimeoutMs = 45000;
    struct Entry { QByteArray data; int code = 0; qint64 at = 0; };
    static QMutex mutex;
    static QHash<QString, Entry> cache;
    constexpr qint64 kTtlMs = 90000;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lock(&mutex);
        const auto it = cache.constFind(url);
        if (it != cache.constEnd() && now - it->at < kTtlMs) return {it->code, it->data, {}};
    }

    const Fetched f = fetch(url, referer, {}, kBuildTimeoutMs, cancel);
    const bool cacheable = f.code >= 200 && f.code < 400 && !f.data.isEmpty()
                           && (f.data.contains("#EXT-X-ENDLIST") || f.data.contains("#EXT-X-STREAM-INF"));
    if (cacheable) {
        QMutexLocker lock(&mutex);
        if (cache.size() > 32) cache.clear();
        cache.insert(url, {f.data, f.code, now});
    }
    return f;
}

static void finish(QTcpSocket &sock) {
    while (sock.bytesToWrite() > 0 && sock.state() == QAbstractSocket::ConnectedState)
        if (!sock.waitForBytesWritten(10000)) break;
    if (sock.state() == QAbstractSocket::ConnectedState) {
        sock.disconnectFromHost();
        if (sock.state() != QAbstractSocket::UnconnectedState)
            sock.waitForDisconnected(3000);
    }
}

static void reply(QTcpSocket &sock, int code, const QByteArray &ctype,
                  const QByteArray &body, bool headOnly = false,
                  const QByteArray &contentRange = {}) {
    const char *phrase = code == 200 ? "OK"
                       : code == 206 ? "Partial Content"
                       : code == 400 ? "Bad Request"
                       : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found"
                       : code == 405 ? "Method Not Allowed"
                       : code == 502 ? "Bad Gateway" : "Error";
    QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + phrase + "\r\n";
    if (!ctype.isEmpty())        head += "Content-Type: " + ctype + "\r\n";
    if (!contentRange.isEmpty()) head += "Content-Range: " + contentRange + "\r\n";
    head += "Accept-Ranges: bytes\r\n";
    head += "Content-Length: " + QByteArray::number(headOnly ? 0 : body.size()) + "\r\n"
            "Connection: close\r\n\r\n";

    if (sock.state() != QAbstractSocket::ConnectedState) return;
    sock.write(head);
    if (!headOnly) sock.write(body);
    finish(sock);
}

void HlsProxy::incomingConnection(qintptr handle) {
    const quint16 port = m_port.load();
    const QByteArray secret = m_secret;   // by value: the job must never touch `this`
    const CancelToken proxyGone = m_cancel;
    m_pool.start([handle, port, secret, proxyGone]() {
        QTcpSocket sock;
        if (!sock.setSocketDescriptor(handle)) {
            Platform::closeSocket(handle);
            return;
        }

        QByteArray req;
        QElapsedTimer age;
        age.start();
        while (!req.contains("\r\n\r\n")) {
            if (req.size() > 8192 || age.elapsed() > 5000) return;
            if (!sock.waitForReadyRead(1000)) {
                if (sock.state() != QAbstractSocket::ConnectedState) return;
                continue;
            }
            req += sock.readAll();
        }

        const QList<QByteArray> parts = req.left(req.indexOf("\r\n")).split(' ');
        if (parts.size() < 2) { reply(sock, 400, {}, {}); return; }
        const QByteArray method = parts[0];
        const QUrl target(QString::fromUtf8(parts[1]));
        const QUrlQuery q(target.query());
        const QString url = q.queryItemValue("u", QUrl::FullyDecoded);
        const QString ref = q.queryItemValue("r", QUrl::FullyDecoded);
        const bool isPlaylist = target.path().endsWith(".m3u8");
        const QByteArray ctype = isPlaylist ? "application/vnd.apple.mpegurl" : "video/mp2t";

        if (method != "GET" && method != "HEAD") { reply(sock, 405, {}, {}); return; }
        if (!sameSignature(sign(secret, url, ref), q.queryItemValue("s").toLatin1())) {
            reply(sock, 403, {}, {});
            return;
        }
        const QString scheme = QUrl(url).scheme();
        if (scheme != "http" && scheme != "https") { reply(sock, 400, {}, {}); return; }
        if (method == "HEAD") { reply(sock, 200, ctype, {}, true); return; }

        QByteArray range;
        for (const QByteArray &line : req.split('\n'))
            if (line.left(6).toLower() == "range:") range = line.mid(6).trimmed();

        const CancelToken hungUp;
        QObject::connect(&sock, &QAbstractSocket::disconnected, [hungUp] { hungUp.cancel(); });

        // A segment streams as it arrives once the first KiB show junk to strip. It goes out with the length upstream gave
        // (less the junk), so one cut short is plainly short; with none, the body ends with the connection.
        constexpr qsizetype kJunkScanBytes = 8192 + 188 * 4;
        const bool streamed = !isPlaylist && range.isEmpty() && !target.path().endsWith(".bin");
        QByteArray held;
        bool started = false;
        qint64 streamedBytes = 0;
        qint64 upstreamLength = -1;
        const auto start = [&] {
            started = true;
            const qsizetype junk = tsOffset(held);
            QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: " + ctype + "\r\n";
            if (upstreamLength > junk) head += "Content-Length: " + QByteArray::number(upstreamLength - junk) + "\r\n";
            sock.write(head + "Connection: close\r\n\r\n");
            sock.write(held.mid(junk));
            held.clear();
        };
        Client::Sink sink;
        if (streamed)
            sink = [&](int, const QMap<QString, QString> &headers, const QByteArray &chunk) {
                if (sock.state() != QAbstractSocket::ConnectedState) return false;
                if (!streamedBytes && Client::headerValue(headers, "content-encoding").isEmpty()) {
                    bool known = false;
                    const qint64 length = Client::headerValue(headers, "content-length").toLongLong(&known);
                    if (known) upstreamLength = length;
                }
                streamedBytes += chunk.size();
                if (started) { sock.write(chunk); return true; }
                held += chunk;
                if (held.size() >= kJunkScanBytes) start();
                return true;
            };

        if (!isPlaylist && isGone(url)) { reply(sock, 404, {}, {}); return; }
        const Fetched f = isPlaylist ? fetchPlaylist(url, ref, proxyGone)
                                     : fetch(url, ref, range, 0, hungUp.composeWith(proxyGone), sink);
        if (!isPlaylist && (f.code == 404 || f.code == 410)) markGone(url);
        if (started || !held.isEmpty()) {
            // Broken off upstream: a reset, which mpv retries, not a short segment that looks whole.
            if (cutOff(f, streamedBytes)) {
                Platform::resetOnClose(sock.socketDescriptor());
                sock.abort();
                return;
            }
            if (!started) start();
            finish(sock);
            return;
        }
        if (f.code < 200 || f.code >= 400) { reply(sock, f.code > 0 ? f.code : 502, {}, {}); return; }
        if (cutOff(f, f.data.size())) { reply(sock, 502, {}, {}); return; }

        QByteArray body = f.data;
        if (isPlaylist)
            body = rewritePlaylist(QString::fromUtf8(f.data), url, ref, port, secret).toUtf8();
        else if (!target.path().endsWith(".bin"))
            if (const int junk = tsOffset(f.data); junk > 0) body = f.data.mid(junk);

        const bool partial = !isPlaylist && f.code == 206;
        reply(sock, partial ? 206 : 200, ctype, body, false, partial ? f.contentRange : QByteArray());
    });
}
