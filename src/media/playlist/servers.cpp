#include "media/playlist/servers.h"
#include "providers/provider.h"
#include "net/cloudflare.h"
#include "app/logger.h"
#include "app/settings.h"
#include "app/exception.h"
#include <QDateTime>
#include <QUrlQuery>
#include <algorithm>
#include <QtConcurrent/QtConcurrentTask>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <QUrl>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

bool ServerListModel::Cached::fresh() const {
    return QDateTime::currentMSecsSinceEpoch() < expiresAtMs;
}

qint64 ServerListModel::expiryOf(const Source &info) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 until = now + 15 * 60;
    if (!info.videos.isEmpty()) {
        const QUrlQuery query(info.videos.first().url);
        for (const char *key : {"deadline", "expires", "Expires", "expire", "exp", "e"}) {
            const qint64 deadline = query.queryItemValue(QLatin1String(key)).toLongLong();
            if (deadline < 1'500'000'000 || deadline > 4'000'000'000) continue;
            until = qMin(deadline - 60, now + 2 * 60 * 60);
            break;
        }
    }
    return until * 1000;
}

ServerListModel::SourceCache ServerListModel::stamped(QHash<QString, Source> &&sources) {
    SourceCache cache;
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const qint64 expires = expiryOf(it.value());
        cache.insert(it.key(), Cached{std::move(it.value()), expires});
    }
    return cache;
}

void ServerListModel::setServers(const QList<VideoServer> &servers, Provider *provider) {
    beginResetModel();
    m_servers = servers;
    m_provider = provider;
    m_sourceCache.clear();
    m_brokenServers.clear();
    m_currentIndex = -1;
    m_hasDub = std::any_of(m_servers.cbegin(), m_servers.cend(),
                           [](const VideoServer &s) { return s.translation == VideoServer::Dub; });
    endResetModel();
    resort();
    emit countChanged();
}

void ServerListModel::setCurrentIndex(int index) {
    if (index == m_currentIndex || !isValidIndex(index)) return;
    m_currentIndex = index;
    emit currentIndexChanged();
}

void ServerListModel::setCurrentServer(const QString &name) {
    setCurrentIndex(indexOfServer(name));
    if (m_provider && !name.isEmpty()) m_provider->setPreferredServer(name);
}

void ServerListModel::resort() {
    const QString currentName = isValidIndex(m_currentIndex) ? m_servers[m_currentIndex].name : QString();
    beginResetModel();
    std::stable_sort(m_servers.begin(), m_servers.end(),
                     [this](const VideoServer &a, const VideoServer &b) {
                         const bool ab = m_brokenServers.contains(a.name);
                         const bool bb = m_brokenServers.contains(b.name);
                         if (ab != bb) return !ab;
                         if (a.translation != b.translation) return a.translation < b.translation;
                         if (a.resolution != b.resolution) return a.resolution > b.resolution;
                         return a.name < b.name;
                     });
    m_currentIndex = currentName.isEmpty() ? -1 : indexOfServer(currentName);
    endResetModel();
    emit currentIndexChanged();
}

int ServerListModel::indexOfServer(const QString &name) const {
    for (int i = 0; i < m_servers.size(); ++i)
        if (m_servers[i].name == name) return i;
    return -1;
}

void ServerListModel::setPreferredServer(int index) {
    if (!m_provider || !isValidIndex(index)) return;
    m_provider->setPreferredServer(m_servers[index].name);
}

VideoServer &ServerListModel::at(int index) {
    Q_ASSERT(isValidIndex(index));
    return m_servers[index];
}

void ServerListModel::clear() {
    beginResetModel();
    m_servers.clear();
    m_currentIndex = -1;
    m_provider = nullptr;
    m_hasDub = false;
    m_sourceCache.clear();
    m_brokenServers.clear();
    endResetModel();
    emit countChanged();
    emit currentIndexChanged();
}

void ServerListModel::setCachedSources(SourceCache cache) {
    m_sourceCache = std::move(cache);
    for (auto it = m_sourceCache.keyBegin(); it != m_sourceCache.keyEnd(); ++it)
        m_brokenServers.remove(*it);
    if (!m_servers.isEmpty())
        emit dataChanged(index(0), index(m_servers.size() - 1), {StatusRole});
}

void ServerListModel::cacheSource(const QString &name, Source info) {
    const qint64 expires = expiryOf(info);
    m_sourceCache.insert(name, Cached{std::move(info), expires});
    const bool wasBroken = m_brokenServers.remove(name);
    if (wasBroken) resort();
    else emitStatusChanged(name);
}

void ServerListModel::dropSource(const QString &name) {
    if (m_sourceCache.remove(name)) emitStatusChanged(name);
}

void ServerListModel::markBroken(const QString &name) {
    if (m_sourceCache.contains(name)) return;
    if (!m_brokenServers.contains(name)) {
        m_brokenServers.insert(name);
        resort();
    }
}

void ServerListModel::emitStatusChanged(const QString &name) {
    if (const int row = indexOfServer(name); row >= 0)
        emit dataChanged(index(row), index(row), {StatusRole});
}

bool ServerListModel::isValidIndex(int index) const {
    return index >= 0 && index < m_servers.size();
}

int ServerListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_servers.size();
}

QVariant ServerListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || !isValidIndex(index.row())) return QVariant();
    const auto &server = m_servers.at(index.row());
    switch (role) {
    case NameRole:
    case Qt::DisplayRole:
        return server.name;
    case LinkRole:
        return server.link;
    case StatusRole:
        if (cachedSource(server.name)) return Working;
        if (m_brokenServers.contains(server.name)) return Broken;
        return Unchecked;
    case TranslationRole:
        return static_cast<int>(server.translation);
    case SectionRole:
        if (m_brokenServers.contains(server.name)) return QStringLiteral("Broken");
        if (!m_hasDub) return QString();
        return server.translation == VideoServer::Dub ? QStringLiteral("Dubbed")
             : server.translation == VideoServer::Sub ? QStringLiteral("Subbed") : QString();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ServerListModel::roleNames() const {
    return {
            {NameRole, "name"},
            {LinkRole, "link"},
            {StatusRole, "status"},
            {TranslationRole, "translation"},
            {SectionRole, "section"},
            {Qt::DisplayRole, "text"},
            };
}

using Playability = ServerSelector::Playability;

namespace {

bool isTransient(int code) {
    return code <= 0 || code == 408 || code == 425 || code == 429
        || code == 500 || code == 502 || code == 503 || code == 504;
}

bool isTransient(const Client::Response &response) {
    return !response.deadHost && isTransient(response.code);
}

// Bypass off, or a dead CDN's 403 opens a solver; capped: a host ignoring Range sends the whole file.
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
        // A refused GET is final even when HEAD says 200; only a host refusing the range or method gets a HEAD.
        if (seg.code == 405 || seg.code == 416 || seg.code == 501) {
            const auto head = probe(client, segUrl, headers, true);
            if (head.code >= 200 && head.code < 400) return Playability::Playable;
            return isTransient(head) ? Playability::Unknown : Playability::Broken;
        }
        return isTransient(seg) ? Playability::Unknown : Playability::Broken;
    }
    return Playability::Unknown;
}

Playability probePlayability(Client *client, Source &playItem) {
    const auto &video = playItem.videos.first();
    const QString url = video.url.toString();

    // A stream can be challenged separately from the site that linked it, and mpv has no jar.
    Cloudflare::applyClearanceHeaders(video.url, playItem.headers);
    const auto &headers = playItem.headers;

    if (video.url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive))
        return checkHls(client, url, headers);

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

// Servers often share one stream (Anikoto's three are one megaplay file), so one probe answers all; only a verdict is shared.
struct SharedVerdict {
    std::shared_future<Playability> verdict;
    qint64 startedMs = 0;
};
std::mutex g_verdictMutex;
QHash<QString, SharedVerdict> g_verdicts;
constexpr qint64 kVerdictTtlMs = 60000;

QString verdictKey(const Source &playItem) {
    QString key = playItem.videos.first().url.toString();
    for (auto it = playItem.headers.constBegin(); it != playItem.headers.constEnd(); ++it)
        key += QLatin1Char('\n') + it.key().toLower() + QLatin1Char(':') + it.value();
    return key;
}

}

Playability ServerSelector::playability(Client *client, Source &playItem) {
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
            Cloudflare::applyClearanceHeaders(playItem.videos.first().url, playItem.headers);
            return verdict;
        }
        return probePlayability(client, playItem);
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

void ServerSelector::forget(const Source &playItem) {
    if (playItem.videos.isEmpty()) return;
    std::lock_guard<std::mutex> lock(g_verdictMutex);
    g_verdicts.remove(verdictKey(playItem));
}

// The race waits on a condition variable, not its futures: a saturated shared pool would deadlock behind itself.
QThreadPool &ServerSelector::probePool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(16); return true; }();
    Q_UNUSED(configured);
    return pool;
}

ServerSelector::Result ServerSelector::findWorkingServer(Client *client, Provider *provider,
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
    QHash<QString, Source> extractedSources;
    int winner = -1;
    Source winnerSource;

    std::mutex failureMutex;
    QString failure;
    QString failureHeader;
    auto rememberFailure = [&](const AppException &e) {
        std::lock_guard<std::mutex> lock(failureMutex);
        if (failure.isEmpty()) { failure = e.message(); failureHeader = e.header(); }
    };

    auto raceRange = [&](int lo, int hi) {
        if (lo >= hi || client->isCancelled()) return;
        CancelToken raceOver;

        std::condition_variable settled;
        int  chosen   = -1;
        int  fallback = -1;
        int  finished = 0;
        bool preferredPending = preferredIndex >= lo && preferredIndex < hi;
        Source fallbackSource;

        auto accept = [&](int index, Source &&source) {
            chosen = index;
            winnerSource = std::move(source);
            raceOver.cancel();
            if (index == preferredIndex)
                logOk() << "Player" << "Using" << (userChose ? "preferred server" : "best quality") << servers[index].name;
            else
                logOk() << "Player" << "Using" << servers[index].name;
        };

        QList<QFuture<void>> jobs;
        jobs.reserve(hi - lo);
        for (int i = lo; i < hi; ++i) {
            if (client->isCancelled()) break;
            jobs.push_back(QtConcurrent::task([&, i]() {
                Client subClient = client->withCancel(raceOver);
                bool playable = false;
                Source source;
                if (!subClient.isCancelled()) {
                    try {
                        source = provider->extractSource(&subClient, servers[i]);
                        source.keepTrustedTracks();
                        const auto verdict = subClient.isCancelled() ? Playability::Unknown
                                                                     : playability(&subClient, source);
                        playable = verdict == Playability::Playable;
                        if (!subClient.isCancelled() && !playable) {
                            if (verdict == Playability::Broken) logWarn() << "Player" << servers[i].name << "is broken";
                            else                                logWarn() << "Player" << servers[i].name << "did not answer";
                        }
                    } catch (AppException &e) {
                        e.log("Player");
                        rememberFailure(e);
                    } catch (const std::exception &e) {
                        logWarn() << "Player" << servers[i].name << e.what();
                    } catch (...) {
                        logWarn() << "Player" << servers[i].name << "unknown error";
                    }
                }

                std::lock_guard<std::mutex> lock(resultMutex);
                ++finished;
                if (playable && !subClient.isCancelled()) {
                    extractedSources.insert(servers[i].name, source);
                    if (chosen < 0) {
                        if (i == preferredIndex || !preferredPending) accept(i, std::move(source));
                        else if (fallback < 0) { fallback = i; fallbackSource = std::move(source); }
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
                if (fallback >= 0 && !preferredPending) { accept(fallback, std::move(fallbackSource)); break; }
                if (fallback >= 0) {
                    if (settled.wait_for(lock, std::chrono::milliseconds(1500)) == std::cv_status::timeout
                        && chosen < 0)
                        accept(fallback, std::move(fallbackSource));
                } else {
                    settled.wait(lock);
                }
            }
            if (chosen < 0 && fallback >= 0) accept(fallback, std::move(fallbackSource));
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
        result.source = std::move(winnerSource);
    }
    result.cachedSources = std::move(extractedSources);
    if (winner < 0) {
        std::lock_guard<std::mutex> lock(failureMutex);
        result.failure = failure;
        result.failureHeader = failureHeader;
    }
    return result;
}
