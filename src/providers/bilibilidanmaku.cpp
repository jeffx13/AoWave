#include "providers/bilibili.h"
#include "core/logger.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <zlib.h>

// DmSegMobileReply; unknown fields skip by wiretype.

namespace {

constexpr int kSegmentMs   = 360000;   // 6-minute chunks
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
    return false;   // overlong varint
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
    // 3/4 are deprecated groups, 6/7 invalid.
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
        } else if (!skipField(p, end, wire)) {
            return false;
        }
    }
    return true;
}

// Raw deflate: qUncompress wants a length prefix and a zlib header.
QByteArray inflateRaw(const QByteArray &input) {
    if (input.isEmpty()) return {};
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return {};

    stream.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = uInt(input.size());

    QByteArray out;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    int status = Z_OK;
    do {
        stream.next_out  = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = uInt(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) break;
        out.append(chunk.constData(), chunk.size() - qsizetype(stream.avail_out));
        // A danmaku document is tens of KB.
        if (out.size() > 32 * 1024 * 1024) { status = Z_DATA_ERROR; break; }
    } while (status != Z_STREAM_END && stream.avail_in > 0);

    inflateEnd(&stream);
    return status == Z_STREAM_END ? out : QByteArray();
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

        if (field == 1 && wire == 2) {          // repeated DanmakuElem
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

// The original API: one deflate XML document, capped at a few thousand comments, but it
// answers for videos seg.so refuses.
QList<DanmakuComment> fetchLegacyXml(Client *client, qint64 cid,
                                     const QMap<QString, QString> &headers,
                                     const QString &proxyApi) {
    Client worker = *client;
    const auto response = fetchBytes(worker, QStringLiteral("https://api.bilibili.com/x/v1/dm/list.so"),
                                     {{"oid", QString::number(cid)}}, headers, proxyApi);
    if (response.code != 200 || response.bytes.isEmpty()) return {};

    // Raw deflate, no wrapper: inflateInit2 with a negative window.
    QByteArray xml = inflateRaw(response.bytes);
    if (xml.isEmpty()) xml = response.bytes;          // some mirrors answer in plain text
    if (!xml.contains("<d ")) return {};

    QList<DanmakuComment> out;
    // The pattern contains `)"`, which would end the default literal.
    static const QRegularExpression entry(
        QStringLiteral(R"RX(<d[^>]*\bp="([^"]*)"[^>]*>(.*?)</d>)RX"),
        QRegularExpression::DotMatchesEverythingOption);
    auto it = entry.globalMatch(QString::fromUtf8(xml));
    while (it.hasNext()) {
        const auto match = it.next();
        const auto fields = match.captured(1).split(QLatin1Char(','));
        if (fields.size() < 5) continue;
        DanmakuComment comment;
        comment.timeMs   = int(fields.at(0).toDouble() * 1000.0);
        comment.mode     = fields.at(1).toInt();
        comment.fontSize = fields.at(2).toInt();
        comment.color    = quint32(fields.at(3).toUInt()) & 0xFFFFFFu;
        comment.weight   = fields.size() > 8 ? fields.at(8).toInt() : 10;
        comment.text     = match.captured(2)
                               .replace(QLatin1String("&amp;"),  QLatin1String("&"))
                               .replace(QLatin1String("&lt;"),   QLatin1String("<"))
                               .replace(QLatin1String("&gt;"),   QLatin1String(">"))
                               .replace(QLatin1String("&quot;"), QLatin1String("\""))
                               .replace(QLatin1String("&apos;"), QLatin1String("'"));
        if (comment.fontSize <= 0) comment.fontSize = 25;
        if (comment.weight   <= 0) comment.weight   = 10;
        if (!comment.text.isEmpty()) out.append(std::move(comment));
    }
    return out;
}

struct SegmentResult {
    QList<DanmakuComment> comments;
    QString refusal;
};

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

SegmentResult fetchSegment(Client *client, qint64 cid, qint64 aid, int index,
                           const QMap<QString, QString> &headers, const QString &proxyApi) {
    QMap<QString, QString> params{{"type", "1"},
                                  {"oid", QString::number(cid)},
                                  {"segment_index", QString::number(index)}};
    // The web player always sends the aid, and the endpoint refuses many videos without it.
    if (aid > 0) params["pid"] = QString::number(aid);

    Client worker = *client;
    const auto response = fetchBytes(
        worker, QStringLiteral("https://api.bilibili.com/x/v2/dm/web/seg.so"), params,
        headers, proxyApi);

    SegmentResult result;
    if (worker.isCancelled()) return result;
    if (response.code <= 0) {
        // No HTTP status: the transport failed or the client was cancelled.
        result.refusal = response.error.isEmpty()
                             ? QStringLiteral("the request did not complete")
                             : response.error;
        return result;
    }
    if (response.code != 200) {
        result.refusal = QStringLiteral("HTTP %1").arg(response.code);
        return result;
    }
    if (response.bytes.isEmpty()) return result;
    result.refusal = danmakuRefusal(response);
    if (result.refusal.isEmpty()) result.comments = parseSegment(response.bytes);
    return result;
}

}

namespace {

// Walks the segments until one comes back empty. `refusal` carries the first reason the
// endpoint gave, which is what separates "this video has none" from "we were turned away".
QList<DanmakuComment> collectSegments(Client *client, qint64 cid, qint64 aid, int durationMs,
                                      const QMap<QString, QString> &headers,
                                      const QString &proxyApi, QString &refusal) {
    // `timelength` is the preview length, so the count is a floor. A segment past the end is empty.
    const int expected = qBound(1, durationMs > 0 ? (durationMs + kSegmentMs - 1) / kSegmentMs : 4,
                                kMaxSegments);

    QList<DanmakuComment> all;
    int index = 1;
    while (index <= kMaxSegments && !client->isCancelled()) {
        // Firing forty requests to discover it ended at six is how you get rate-limited.
        const int batch = index == 1 ? expected : 1;
        QList<QFuture<SegmentResult>> futures;
        futures.reserve(batch);
        for (int i = index; i < index + batch; ++i)
            futures << QtConcurrent::run([client, cid, aid, i, headers, proxyApi] {
                return fetchSegment(client, cid, aid, i, headers, proxyApi);
            });

        bool lastWasEmpty = false;
        for (auto &future : futures) {
            const SegmentResult part = future.result();
            lastWasEmpty = part.comments.isEmpty();
            all.append(part.comments);
            if (refusal.isEmpty()) refusal = part.refusal;
        }
        index += batch;
        if (lastWasEmpty) break;
    }
    return all;
}

}

QList<DanmakuComment> Bilibili::fetchDanmaku(Client *client, qint64 cid, qint64 aid, int durationMs,
                                            const QMap<QString, QString> &headers,
                                            const QString &proxyApi) {
    if (!client || cid <= 0) return {};

    // Danmaku is public, so an account can only ever narrow what comes back. A jar whose
    // bili_ticket has lapsed reads as a replayed session and gets -352; a relay that rewrites
    // the body loses the protobuf. Neither is a property of the video, so a run that finds
    // nothing is repeated as a plain anonymous visitor before the video is believed empty.
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
        // The XML endpoint answers for some videos seg.so refuses.
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
