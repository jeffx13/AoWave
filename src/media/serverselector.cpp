#include "media/serverselector.h"
#include "shows/showprovider.h"
#include "net/cloudflare.h"
#include "core/logger.h"
#include "core/settings.h"
#include <QtConcurrent/QtConcurrentTask>
#include <QDateTime>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <algorithm>
#include <QUrl>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include "core/exception.h"

using Playability = ServerSelector::Playability;

namespace {

// "Ask again", not "gone", including HlsProxy's 502: one flaky host among healthy siblings.
bool isTransient(int code) {
    return code <= 0 || code == 408 || code == 425 || code == 429
        || code == 500 || code == 502 || code == 503 || code == 504;
}

// A host with no DNS entry answers code 0 like a timeout, but only one is worth a retry.
bool isTransient(const Client::Response &response) {
    return !response.deadHost && isTransient(response.code);
}

// Bypass off, or a dead CDN's 403 opens a solver. Bodies capped: a host ignoring Range
// would hand over the whole file.
Client::Response probe(Client *client, const QString &url,
                       QMap<QString, QString> headers, bool head,
                       const QString &range = {}) {
    if (!range.isEmpty()) headers.insert("Range", range);
    Client prober = *client;
    prober.setBypassEnabled(false);
    prober.setMaxBodyBytes(256 * 1024);
    if (QUrl(url).host() == QLatin1String("127.0.0.1")) prober.setTimeout(45000);

    Client::Response response;
    QElapsedTimer timer;
    for (int attempt = 0; attempt < 3; ++attempt) {
        timer.start();
        response = head ? prober.head(url, headers) : prober.get(url, headers);
        if (!isTransient(response) || prober.isCancelled()) break;
        // A slow failure is a dead host, and retrying stalls the race.
        if (timer.elapsed() > 2000) break;
        if (attempt < 2) QThread::msleep(200 << attempt);
    }
    return response;
}

template <typename Resolve>
Playability keyReachability(Client *client, const QString &body, const QMap<QString, QString> &headers,
                            const Resolve &resolve) {
    static const QRegularExpression keyRe(QStringLiteral("#EXT-X-KEY:([^\r\n]+)"));
    const auto keyMatch = keyRe.match(body);
    if (!keyMatch.hasMatch()) return Playability::Playable;

    static const QRegularExpression uriRe(QStringLiteral("URI=\"([^\"]+)\"|URI=([^\\s,]+)"));
    const auto uriMatch = uriRe.match(keyMatch.captured(1));
    if (!uriMatch.hasMatch()) return Playability::Playable;

    const QString keyUri = uriMatch.captured(1).isEmpty() ? uriMatch.captured(2) : uriMatch.captured(1);
    const QString keyUrl = resolve(keyUri);
    const auto head = probe(client, keyUrl, headers, true);
    if (head.code >= 200 && head.code < 400) return Playability::Playable;
    if (head.deadHost) return Playability::Broken;
    const auto get = probe(client, keyUrl, headers, false);
    if (get.code >= 200 && get.code < 400 && !get.body.isEmpty()) return Playability::Playable;
    return (isTransient(head) || isTransient(get)) ? Playability::Unknown : Playability::Broken;
}

QString firstUriAfter(const QStringList &lines, const QString &marker) {
    for (int i = 0; i < lines.size(); ++i) {
        if (!marker.isEmpty() && !lines[i].trimmed().startsWith(marker)) continue;
        for (int j = i + (marker.isEmpty() ? 0 : 1); j < lines.size(); ++j) {
            const QString u = lines[j].trimmed();
            if (!u.isEmpty() && !u.startsWith('#')) return u;
        }
        return {};
    }
    return {};
}

// An intact playlist with 404 segments otherwise fakes working.
Playability checkHls(Client *client, const QString &url, const QMap<QString, QString> &headers) {
    QString target = url;
    for (int depth = 0; depth < 2; ++depth) {
        const auto pl = probe(client, target, headers, false, QStringLiteral("bytes=0-131071"));
        if (isTransient(pl)) return Playability::Unknown;
        if (pl.code < 200 || pl.code >= 400) return Playability::Broken;
        if (!pl.body.startsWith("#EXTM3U")) return Playability::Playable;

        const QUrl base(target);
        auto resolve = [&base](const QString &u) {
            return (QUrl(u).scheme().isEmpty() ? base.resolved(QUrl(u)) : QUrl(u)).toString();
        };
        if (const auto key = keyReachability(client, pl.body, headers, resolve);
            key != Playability::Playable)
            return key;

        const QStringList lines = pl.body.split('\n');
        if (const QString variant = firstUriAfter(lines, QStringLiteral("#EXT-X-STREAM-INF")); !variant.isEmpty()) {
            target = resolve(variant);
            continue;
        }

        const QString segment = firstUriAfter(lines, {});
        if (segment.isEmpty()) return Playability::Broken;
        const QString segUrl = resolve(segment);
        const auto seg = probe(client, segUrl, headers, false, QStringLiteral("bytes=0-0"));
        if (seg.code == 200 || seg.code == 206) return Playability::Playable;
        if (seg.deadHost) return Playability::Broken;
        const auto head = probe(client, segUrl, headers, true);
        if (head.code >= 200 && head.code < 400) return Playability::Playable;
        return (isTransient(seg) || isTransient(head)) ? Playability::Unknown : Playability::Broken;
    }
    return Playability::Unknown;
}

Playability probePlayability(Client *client, PlayInfo &playItem) {
    const auto &video = playItem.videos.first();
    const QString url = video.url.toString();

    // A stream can be challenged separately from the site that linked it, and mpv has no jar.
    Cloudflare::applyClearanceHeaders(video.url, playItem.headers);
    const auto &headers = playItem.headers;

    // Match the path: proxied urls carry the upstream in a query string.
    if (video.url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return checkHls(client, url, headers);

    // One ranged GET gives status, type and the first bytes. HEAD is the fallback.
    auto resp = probe(client, url, headers, false, QStringLiteral("bytes=0-1023"));
    Cloudflare::applyClearanceHeaders(video.url, playItem.headers);
    if (resp.deadHost) return Playability::Broken;
    if (resp.code == 405 || resp.code == 501) {
        resp = probe(client, url, headers, true);
        if (resp.deadHost) return Playability::Broken;
    }
    if (isTransient(resp)) return Playability::Unknown;
    if (resp.code >= 400) return Playability::Broken;

    const QString contentType = resp.header("Content-Type").toLower();
    if (contentType.contains("mpegurl") || resp.body.startsWith(QLatin1String("#EXTM3U")))
        return checkHls(client, url, headers);
    return Playability::Playable;
}

// Servers often resolve to one stream (Anikoto's three are one megaplay file), and the race,
// the background cache and the next-episode prefetch each ask about it within seconds: one
// probe answers them all. Only a verdict is shared; an inconclusive probe is not.
struct SharedVerdict {
    std::shared_future<Playability> verdict;
    qint64 startedMs = 0;
};
std::mutex g_verdictMutex;
QHash<QString, SharedVerdict> g_verdicts;
constexpr qint64 kVerdictTtlMs = 60000;

QString verdictKey(const PlayInfo &playItem) {
    QString key = playItem.videos.first().url.toString();
    for (auto it = playItem.headers.constBegin(); it != playItem.headers.constEnd(); ++it)
        key += QLatin1Char('\n') + it.key().toLower() + QLatin1Char(':') + it.value();
    return key;
}

}

Playability ServerSelector::playability(Client *client, PlayInfo &playItem) {
    // An empty extraction says nothing: the embed page may have been throttled.
    if (playItem.videos.isEmpty()) return Playability::Unknown;
    if (playItem.videos.first().url.isLocalFile()) return Playability::Playable;

    const QString key = verdictKey(playItem);
    std::promise<Playability> mine;
    std::shared_future<Playability> theirs;
    {
        std::lock_guard<std::mutex> lock(g_verdictMutex);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = g_verdicts.begin(); it != g_verdicts.end();)
            it = now - it->startedMs > kVerdictTtlMs ? g_verdicts.erase(it) : std::next(it);
        if (const auto it = g_verdicts.constFind(key); it != g_verdicts.constEnd())
            theirs = it->verdict;
        else
            g_verdicts.insert(key, {mine.get_future().share(), now});
    }

    if (theirs.valid()) {
        while (theirs.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            if (client->isCancelled()) return Playability::Unknown;
        if (const Playability verdict = theirs.get(); verdict != Playability::Unknown) {
            // The prober stamped the clearance onto its own copy, not this one.
            Cloudflare::applyClearanceHeaders(playItem.videos.first().url, playItem.headers);
            return verdict;
        }
        return probePlayability(client, playItem);   // theirs was cancelled or inconclusive
    }

    Playability verdict = Playability::Unknown;
    try {
        verdict = probePlayability(client, playItem);
    } catch (...) {
        mine.set_value(Playability::Unknown);
        std::lock_guard<std::mutex> lock(g_verdictMutex);
        g_verdicts.remove(key);
        throw;
    }
    mine.set_value(verdict);
    if (verdict == Playability::Unknown) {
        std::lock_guard<std::mutex> lock(g_verdictMutex);
        g_verdicts.remove(key);
    }
    return verdict;
}

// The race waits on a condition variable, not its futures, so a saturated shared pool
// would deadlock behind itself.
QThreadPool &ServerSelector::probePool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(16); return true; }();
    Q_UNUSED(configured);
    return pool;
}

ServerSelector::Result ServerSelector::findWorkingServer(Client *client, ShowProvider *provider,
                                                         QList<VideoServer> &servers, Priority priority) {
    Result result;

    const auto want = Settings::instance().preferDub() ? VideoServer::Dub : VideoServer::Sub;
    const bool hasPreferredLang =
        std::any_of(servers.begin(), servers.end(),
                    [want](const VideoServer &s) { return s.translation == want; });

    QString preferred = provider->preferredServer();
    const bool userChose = !preferred.isEmpty();
    if (!userChose) {
        int best = -1;
        for (int i = 0; i < servers.size(); ++i) {
            if (hasPreferredLang && servers[i].translation != want) continue;
            if (servers[i].resolution == 0) continue;
            if (best < 0 || servers[i].resolution > servers[best].resolution) best = i;
        }
        if (best >= 0) preferred = servers[best].name;
    }

    const auto rest = std::stable_partition(servers.begin(), servers.end(),
                                            [want](const VideoServer &s) { return s.translation == want; });
    int pivot = int(std::distance(servers.begin(), rest));
    if (pivot == 0) pivot = servers.size();

    int preferredIndex = -1;
    if (!preferred.isEmpty()) {
        const auto it = std::find_if(servers.begin(), servers.end(),
                                     [&](const VideoServer &s) { return s.name == preferred; });
        if (it != servers.end() && (it->translation == want || !hasPreferredLang))
            preferredIndex = int(std::distance(servers.begin(), it));
    }

    std::mutex resultMutex;
    QHash<QString, PlayInfo> extractedSources;
    int winner = -1;
    PlayInfo winnerPlayInfo;

    // So the caller can show it instead of "nothing worked".
    std::mutex failureMutex;
    QString failure;
    QString failureHeader;
    auto rememberFailure = [&](const AppException &e) {
        std::lock_guard<std::mutex> lock(failureMutex);
        if (failure.isEmpty()) { failure = e.message(); failureHeader = e.header(); }
    };

    // Another playable server is held as a fallback until the preferred one fails.
    auto raceRange = [&](int lo, int hi) {
        if (lo >= hi || client->isCancelled()) return;
        CancelToken raceOver;

        std::condition_variable settled;
        int  chosen   = -1;
        int  fallback = -1;
        int  finished = 0;
        bool preferredPending = preferredIndex >= lo && preferredIndex < hi;
        PlayInfo fallbackPlayInfo;

        auto accept = [&](int index, PlayInfo &&playInfo) {
            chosen = index;
            winnerPlayInfo = std::move(playInfo);
            raceOver.cancel();
            if (index == preferredIndex)
                logOk() << "Server" << "Using" << (userChose ? "preferred server" : "best quality") << servers[index].name;
            else
                logOk() << "Server" << "Using" << servers[index].name;
        };

        QList<QFuture<void>> jobs;
        jobs.reserve(hi - lo);
        for (int i = lo; i < hi; ++i) {
            if (client->isCancelled()) break;
            jobs.push_back(QtConcurrent::task([&, i]() {
                Client subClient = client->withCancel(raceOver);
                bool playable = false;
                PlayInfo playInfo;
                if (!subClient.isCancelled()) {
                    try {
                        playInfo = provider->extractSource(&subClient, servers[i]);
                        const auto verdict = subClient.isCancelled() ? Playability::Unknown
                                                                     : playability(&subClient, playInfo);
                        playable = verdict == Playability::Playable;
                        if (!subClient.isCancelled() && !playable) {
                            if (verdict == Playability::Broken) logWarn() << "Server" << servers[i].name << "is broken";
                            else                                logWarn() << "Server" << servers[i].name << "did not answer";
                        }
                    } catch (AppException &e) {
                        e.log();
                        rememberFailure(e);
                    } catch (const std::exception &e) {
                        logWarn() << "Server" << servers[i].name << e.what();
                    } catch (...) {
                        logWarn() << "Server" << servers[i].name << "unknown error";
                    }
                }

                std::lock_guard<std::mutex> lock(resultMutex);
                ++finished;
                if (playable && !subClient.isCancelled()) {
                    extractedSources.insert(servers[i].name, playInfo);
                    if (chosen < 0) {
                        if (i == preferredIndex || !preferredPending) accept(i, std::move(playInfo));
                        else if (fallback < 0) { fallback = i; fallbackPlayInfo = std::move(playInfo); }
                    }
                }
                if (i == preferredIndex) preferredPending = false;
                settled.notify_all();
            }).onThreadPool(probePool()).withPriority(priority).spawn());
        }

        {
            std::unique_lock<std::mutex> lock(resultMutex);
            const int total = int(jobs.size());
            while (chosen < 0 && finished < total) {
                if (fallback >= 0 && !preferredPending) { accept(fallback, std::move(fallbackPlayInfo)); break; }
                if (fallback >= 0) {
                    if (settled.wait_for(lock, std::chrono::milliseconds(1500)) == std::cv_status::timeout
                        && chosen < 0)
                        accept(fallback, std::move(fallbackPlayInfo));
                } else {
                    settled.wait(lock);
                }
            }
            if (chosen < 0 && fallback >= 0) accept(fallback, std::move(fallbackPlayInfo));
            winner = chosen;
        }

        for (auto &job : jobs)
            job.waitForFinished();
    };

    raceRange(0, pivot);
    if (winner < 0)
        raceRange(pivot, servers.size());

    result.index = winner;
    if (winner >= 0) {
        std::lock_guard<std::mutex> lock(resultMutex);
        result.playInfo = std::move(winnerPlayInfo);
    }
    result.cachedSources = std::move(extractedSources);
    if (winner < 0) {
        std::lock_guard<std::mutex> lock(failureMutex);
        result.failure = failure;
        result.failureHeader = failureHeader;
    }
    return result;
}