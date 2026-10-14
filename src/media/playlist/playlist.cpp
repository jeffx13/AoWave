#include "media/playlist/playlist.h"
#include <cmath>
#include "app/async.h"
#include "app/logger.h"
#include "app/exception.h"
#include "media/player/mpvplayer.h"
#include "providers/provider.h"
#include "providers/providerlist.h"
#include "app/mainwindow.h"
#include "media/playlist/servers.h"
#include "net/cloudflare.h"
#include "media/playlist/localmedia.h"
#include "media/player/danmaku.h"
#include "app/settings.h"
#include "media/playlist/ytdlp.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QtConcurrent/QtConcurrentTask>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFile>
#include <optional>
#include <algorithm>
#include <QMetaObject>

Playlist::Playlist(QObject *parent) : QAbstractItemModel(parent) {
    registerPlaylist(m_root);
    connect(&m_folderWatcher, &QFileSystemWatcher::directoryChanged, this, &Playlist::onLocalDirectoryChanged);
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(500);
    connect(&m_rescanTimer, &QTimer::timeout, this, &Playlist::rescanChangedFolders);
    connect(&m_watcher, &QFutureWatcher<Source>::finished, this, &Playlist::onPlayFinished);
    connect(&m_watcher, &QFutureWatcher<Source>::started,  this, &Playlist::isLoadingChanged);
    connect(&m_watcher, &QFutureWatcher<Source>::finished, this, &Playlist::isLoadingChanged);

    m_prefetchTimer.setSingleShot(true);
    m_prefetchTimer.setInterval(4000);
    connect(&m_prefetchTimer, &QTimer::timeout, this, &Playlist::startNextEpisodePrefetch);
    connect(&m_bgCacheWatcher, &QFutureWatcher<void>::finished, this, [this]() {
        if (std::exchange(m_bgCachePending, false)) cacheRemainingServers();
    });
    connect(&m_progressSync, &ProgressSync::remoteResumeResolved, this, [this](QString link, double seconds) {
        m_remoteAheadLink = link;
        m_remoteAheadAt = seconds;
        emit remoteResumeResolved(link, seconds);
    });
}

void Playlist::onPlayFinished() {
    bool handedToPlayer = false;
    const bool cancelled = m_cancel.isCancelled();
    if (!cancelled) {
        try {
            auto playItem = m_watcher.result();
            if (!playItem.videos.isEmpty()) {
                openResolved(playItem);
                handedToPlayer = true;
            }
        } catch (AppException &ex) {
            if (m_recovering) logWarn() << "Player" << ex.what();
            else ex.report();
        } catch (const std::exception &ex) {
            if (m_recovering) logWarn() << "Player" << ex.what();
            else MainWindow::instance().reportError(ex.what(), tr("Playlist error"));
        } catch (...) {
            MainWindow::instance().reportError(tr("Unknown error."), tr("Playlist error"));
        }
    }
    if (!handedToPlayer) {
        if (m_recovering && !cancelled && !m_pendingItem && m_pendingServerIndex < 0)
            QMetaObject::invokeMethod(this, [this]() { recoverStream({}, -1, false); }, Qt::QueuedConnection);
        else if (!m_recovering)
            setLoadingItem({});
    }

    if (m_pendingServerIndex >= 0) {
        int idx = m_pendingServerIndex;
        m_pendingServerIndex = -1;
        loadServer(idx);
    } else if (m_pendingItem) {
        auto item = m_pendingItem;
        m_pendingItem.clear();
        tryPlay(item);
    }
}

Playlist::~Playlist() {
    disconnect(&m_watcher, &QFutureWatcher<Source>::finished, this, nullptr);
    m_cancel.cancel();
    m_linkCheck.cancel();
    m_queueCheck.cancel();
    m_appendCancel.cancel();
    m_bgCacheCancel.cancel();
    m_prefetchCancel.cancel();
    waitFor(m_prefetchFuture, "Playlist prefetch");
    waitFor(m_watcher,        "Playlist play");
    waitFor(m_appendFuture,   "Playlist append");
    waitFor(m_bgCacheFuture,  "Playlist server cache");
}

QSharedPointer<PlaylistItem> Playlist::find(const QString &link) const {
    const auto it = m_byLink.constFind(link);
    return it != m_byLink.constEnd() ? it.value().toStrongRef() : nullptr;
}

int Playlist::append(const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent) {
    return insert(INT_MAX, playlist, parent);
}

int Playlist::insert(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent) {
    if (!playlist) return -1;
    auto actualParent = parent ? parent : m_root;
    if (!actualParent->isList()) return -1;

    auto existingPlaylist = find(playlist->link);
    if (existingPlaylist)
        return existingPlaylist->row();

    registerPlaylist(playlist);
    index = qBound(0, index, actualParent->count());
    beginInsertRows(indexFor(actualParent.data()), index, index);
    actualParent->insert(index, playlist);
    endInsertRows();
    return index;
}

bool Playlist::isPlaying(const QString &link) const {
    if (link.isEmpty()) return false;
    for (auto item = m_currentItem.toStrongRef(); item; item = item->parent())
        if (item->link == link) return true;
    return false;
}

bool Playlist::relink(const QString &oldLink, const QString &newLink) {
    if (oldLink.isEmpty() || newLink.isEmpty() || oldLink == newLink) return false;
    auto playlist = find(oldLink);
    if (!playlist) return false;
    if (m_byLink.contains(newLink)) {
        logWarn() << "Library" << newLink << "is already open; leaving" << oldLink << "as it is";
        return false;
    }
    m_byLink.remove(oldLink);
    playlist->link = newLink;
    m_byLink.insert(newLink, playlist.toWeakRef());
    logInfo() << "Library" << "relinked" << oldLink << "->" << newLink;
    return true;
}

void Playlist::rekey(const QString &oldLink, const QSharedPointer<PlaylistItem> &newPlaylist) {
    auto stale = find(oldLink);
    if (!stale) return;
    auto parent = stale->parent();
    if (newPlaylist && replace(stale->row(), newPlaylist, parent == m_root ? nullptr : parent) != -1)
        return;
    remove(indexFor(stale.data()));
}

int Playlist::replace(int index, const QSharedPointer<PlaylistItem> &playlist, const QSharedPointer<PlaylistItem> &parent) {
    if (!playlist) return -1;

    auto existingPlaylist = find(playlist->link);
    if (existingPlaylist)
        return existingPlaylist->row();

    if (parent) {
        auto existingParent = find(parent->link);
        if (existingParent && existingParent != parent) return -1;
    }
    auto actualParent = parent ? parent : m_root;
    if (!actualParent->isList()) return -1;
    if (!actualParent->isValidIndex(index)) {
        logError() << "Player" << "Invalid index:" << index << "to replace";
        return -1;
    }

    deregisterPlaylist(actualParent->at(index));
    registerPlaylist(playlist);

    auto currentItem = m_currentItem.toStrongRef();
    bool currentPlaylistReplaced = currentItem && currentItem->parent() == actualParent->at(index);
    if (currentPlaylistReplaced)
        saveProgress();

    beginRemoveRows(indexFor(actualParent.data()), index, index);
    actualParent->removeAt(index);
    endRemoveRows();
    beginInsertRows(indexFor(actualParent.data()), index, index);
    actualParent->insert(index, playlist);
    endInsertRows();

    if (currentPlaylistReplaced)
        setCurrentItem(playlist->currentItem());

    return index;
}

bool Playlist::canRemove(const PlaylistItem *item) const {
    if (!item || !item->parent()) return false;
    if (!item->isList() && !(item->type & PlaylistItem::Pasted)) return false;
    for (auto busy : {m_currentItem.toStrongRef(), m_loadingItem})
        for (; busy; busy = busy->parent())
            if (busy.data() == item) return false;
    return true;
}

void Playlist::remove(const QModelIndex &index) {
    auto *item = static_cast<PlaylistItem*>(index.internalPointer());
    if (!canRemove(item)) return;
    auto keep = item->sharedFromThis();
    auto parent = item->parent();
    const int row = item->row();

    if (item->isList())
        deregisterPlaylist(keep);

    beginRemoveRows(indexFor(parent.data()), row, row);
    parent->removeAt(row);
    endRemoveRows();

    if (parent->isEmpty() && parent != m_root) {
        if (auto grandparent = parent->parent()) {
            deregisterPlaylist(parent);
            const int parentRow = parent->row();
            beginRemoveRows(indexFor(grandparent.data()), parentRow, parentRow);
            grandparent->removeAt(parentRow);
            endRemoveRows();
        }
    }
}

void Playlist::clear() {
    QList<QSharedPointer<PlaylistItem>> kept;
    for (const auto &playlist : m_root->children()) {
        if (canRemove(playlist.data())) deregisterPlaylist(playlist);
        else kept << playlist;
    }
    beginResetModel();
    m_root->clear();
    for (const auto &playlist : std::as_const(kept)) m_root->append(playlist);
    endResetModel();
    m_root->setCurrentIndex(-1);

    setCurrentItem(m_currentItem.toStrongRef());
}

bool Playlist::playShowNow(const QSharedPointer<PlaylistItem> &playlist, int episodeIndex) {
    if (!playlist || !playlist->isList()) return false;
    playlist->season = kQuickPlaySeason;

    const int row = insert(0, playlist);
    if (row < 0) return false;

    m_quickPlayPending = playlist;

    int target = episodeIndex;
    if (!playlist->isValidIndex(target))
        target = playlist->isValidIndex(playlist->currentIndex()) ? playlist->currentIndex() : 0;
    return tryPlay(playlist->at(target));
}

void Playlist::dropOtherQuickPlayRows(const QString &keepLink) {
    const auto rows = m_root->children();
    for (const auto &row : rows)
        if (row->season == kQuickPlaySeason && row->link != keepLink)
            remove(indexFor(row.data()));
}

bool Playlist::playAt(int index) {
    if (m_root->isEmpty()) return false;
    auto playlist = m_root->at(index);
    if (!playlist) return false;
    int itemIndex = (playlist->currentIndex() == -1) ? 0 : playlist->currentIndex();
    return tryPlay(playlist->at(itemIndex));
}

void Playlist::stepItem(int offset) {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem) {
        tryPlay(m_root->at(0));
        return;
    }
    auto playlist = currentItem->parent();
    if (!playlist) {
        logError() << "Player" << currentItem->link << "does not belong to a playlist";
        return;
    }

    int nextItemIndex = currentItem->row() + offset;

    auto parentPlaylist = playlist->parent();
    if (parentPlaylist) {
        int playlistIndex = playlist->row();
        if (nextItemIndex == playlist->count() && playlistIndex + 1 < parentPlaylist->count()) {
            stepPlaylist(1);
            return;
        } else if (nextItemIndex < 0 && playlistIndex - 1 >= 0) {
            stepPlaylist(-1);
            return;
        }
    }
    tryPlay(playlist->at(nextItemIndex));
}

void Playlist::stepPlaylist(int offset) {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem || !currentItem->parent()) {
        tryPlay(m_root->at(0));
        return;
    }
    auto playlist = currentItem->parent();
    auto parentPlaylist = playlist->parent();
    if (!parentPlaylist) return;

    auto nextPlaylist = parentPlaylist->at(playlist->row() + offset);
    if (!nextPlaylist) return;

    int nextItemIndex = 0;
    if (nextPlaylist->currentIndex() != -1) {
        nextItemIndex = nextPlaylist->currentIndex();
    } else {
        for (int i = 0; i < nextPlaylist->count(); ++i) {
            if (!nextPlaylist->at(i)->isList()) {
                nextItemIndex = i;
                break;
            }
        }
    }
    tryPlay(nextPlaylist->at(nextItemIndex));
}

void Playlist::loadIndex(const QModelIndex &index) {
    auto item = static_cast<PlaylistItem*>(index.internalPointer());
    if (item && item->solo) item = item->at(0).data();
    if (!item || item->isList()) return;
    auto playlist = item->parent();
    if (playlist) tryPlay(playlist->at(item->row()));
}

void Playlist::reload() {
    auto item = m_loadingItem ? m_loadingItem : m_currentItem.toStrongRef();
    if (!item || item->isList()) return;
    auto *mpv = MpvPlayer::instance();
    const bool playing = mpv && !m_loadingItem && mpv->preciseDuration() > 0;
    if (playing && m_remoteAheadLink == item->link && m_remoteAheadAt > mpv->preciseTime() + 5) {
        mpv->seek(std::exchange(m_remoteAheadAt, -1), true);
        m_remoteAheadLink.clear();
        return;
    }
    m_recovery = Recovery{item->link};
    m_recovery.reason = tr("reloaded");
    m_recovery.at = playing ? mpv->preciseTime() : -1;
    m_recovery.fraction = playing ? -1 : item->progress();
    m_recovery.reextracted = true;
    m_recovering = false;
    logInfo() << "Player" << "Reloading" << item->displayName.simplified() << "from"
              << (playing ? QString::number(m_recovery.at, 'f', 1) + "s"
                          : QString::number(m_recovery.fraction * 100, 'f', 1) + "%");

    const QString server = m_serversFor == item->link ? m_serverListModel.currentName() : QString();
    if ((item->type & PlaylistItem::Online) && !server.isEmpty()) {
        forgetSource(server);
        setLoadingItem(item);
        resolveServer(m_serverListModel.currentIndex(), m_recovery.at, m_recovery.fraction, true);
        return;
    }
    m_resumeAt = m_recovery.at;
    m_resumeFraction = m_recovery.fraction;
    tryPlay(item);
}

void Playlist::loadServer(int index) {
    double at = -1;
    if (auto *mpv = MpvPlayer::instance(); mpv && mpv->preciseDuration() > 0) at = mpv->preciseTime();
    m_recovering = false;
    resolveServer(index, at, -1, false);
}

void Playlist::resolveServer(int index, double at, double fraction, bool fresh) {
    if (m_watcher.isRunning()) {
        m_pendingServerIndex = index;
        m_cancel.cancel();
        return;
    }
    if (!m_serverListModel.isValidIndex(index)) return;
    VideoServer server = m_serverListModel.at(index);
    Provider *provider = m_serverListModel.provider();
    if (!provider) return;

    logInfo() << "Player" << "Switching to" << server.name;

    if (const auto *cached = fresh ? nullptr : m_serverListModel.cachedSource(server.name)) {
        Source playItem = *cached;
        if (!playItem.videos.isEmpty())
            Cloudflare::applyClearanceHeaders(playItem.videos.first().url, playItem.headers);
        m_resumeAt = at;
        m_resumeFraction = fraction;
        m_serverListModel.setCurrentIndex(index);
        m_serverListModel.setPreferredServer(index);
        openResolved(playItem);
        logInfo() << "Player" << "Loaded" << server.name << "(cached)";
        return;
    }

    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run([this, server, provider, at, fraction, cancel = m_cancel]() {
        Client client(cancel);
        Source playItem = provider->extractSource(&client, server);
        playItem.keepTrustedTracks();
        const auto verdict = ServerSelector::playability(&client, playItem);
        if (verdict != ServerSelector::Playability::Playable && !client.isCancelled()) {
            playItem.clear();
            if (verdict == ServerSelector::Playability::Broken) {
                logWarn() << "Player" << server.name << "is broken";
                QMetaObject::invokeMethod(this, [this, name = server.name, cancel]() {
                    if (!cancel.isCancelled()) m_serverListModel.markBroken(name);
                }, Qt::QueuedConnection);
            } else {
                logWarn() << "Player" << server.name << "did not answer - left unchecked";
            }
        }
        if (playItem.videos.isEmpty()) {
            logWarn() << "Player" << QString("Failed to load server %1").arg(server.name);
            return playItem;
        }
        playItem.startSeconds = at > 0 ? at : 0;
        playItem.progress = at > 0 ? 0 : qMax(0.0, fraction);
        QMetaObject::invokeMethod(this, [this, serverName = server.name, playItem, cancel]() {
            if (cancel.isCancelled()) return;
            m_serverListModel.cacheSource(serverName, playItem);
            m_serverListModel.setCurrentServer(serverName);
            logInfo() << "Player" << "Loaded" << serverName;
        }, Qt::QueuedConnection);
        return playItem;
    }));
    emit isLoadingChanged();
}

static bool moveToAnotherHost(Source &info) {
    bool moved = false;
    const auto rotate = [&moved](Track &track) {
        if (track.alternates.isEmpty()) return;
        track.alternates.append(track.url);
        track.url = track.alternates.takeFirst();
        moved = true;
    };
    for (Video &video : info.videos) rotate(video);
    for (Track &audio : info.audios) rotate(audio);
    return moved;
}

void Playlist::forgetSource(const QString &server) {
    if (const Source *cached = m_serverListModel.cachedSource(server)) ServerSelector::forget(*cached);
    ServerSelector::forget(m_openedInfo);
    m_serverListModel.dropSource(server);
}

void Playlist::reopen(const QSharedPointer<PlaylistItem> &item, Source info) {
    setLoadingItem(item);
    m_resumeAt = m_recovery.at;
    m_resumeFraction = m_recovery.fraction;
    openResolved(info);
}

void Playlist::recoverStream(const QString &reason, double at, bool fromPlayer) {
    if (m_watcher.isRunning()) return;
    auto item = m_loadingItem ? m_loadingItem : m_currentItem.toStrongRef();
    if (!item || item->isList() || item->preview) { m_recovering = false; return; }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_recovery.link != item->link || now - m_recovery.lastFailureMs > kRecoveryWindowMs) {
        m_recovery = Recovery{item->link};
        m_recovery.at = m_openedInfo.startSeconds > 0 ? m_openedInfo.startSeconds : -1;
        m_recovery.fraction = m_openedInfo.startSeconds > 0 ? -1 : m_openedInfo.progress;
    }
    m_recovery.lastFailureMs = now;
    if (!reason.isEmpty()) m_recovery.reason = reason;
    if (at > 1) {
        m_recovery.at = at;
        m_recovery.fraction = -1;
    }
    m_recovering = true;

    const QString label = item->displayName.simplified();
    const bool online = item->type & PlaylistItem::Online;
    const QString server = online && m_serversFor == item->link ? m_serverListModel.currentName() : QString();

    if (fromPlayer && m_recovery.hostMoves < kMaxHostMoves && moveToAnotherHost(m_openedInfo)) {
        ++m_recovery.hostMoves;
        logWarn() << "Player" << label << "failed (" + m_recovery.reason + "); trying another host";
        if (!server.isEmpty()) m_serverListModel.cacheSource(server, m_openedInfo);
        reopen(item, m_openedInfo);
        return;
    }
    if (fromPlayer && !m_recovery.reextracted) {
        m_recovery.reextracted = true;
        if (!server.isEmpty()) {
            logWarn() << "Player" << label << "failed (" + m_recovery.reason + "); asking" << server << "again";
            forgetSource(server);
            resolveServer(m_serverListModel.currentIndex(), m_recovery.at, m_recovery.fraction, true);
        } else if (!online) {
            logWarn() << "Player" << label << "failed (" + m_recovery.reason + "); opening it again";
            reopen(item, m_openedInfo);
        } else {
            m_streamInstead = item->link;
            m_resumeAt = m_recovery.at;
            m_resumeFraction = m_recovery.fraction;
            setLoadingItem({});
            m_recovering = false;
            tryPlay(item);
        }
        return;
    }
    if (!server.isEmpty()) {
        m_recovery.triedServers.insert(server);
        forgetSource(server);
        for (int i = 0; i < m_serverListModel.count(); ++i) {
            const QString name = m_serverListModel.at(i).name;
            if (m_recovery.triedServers.contains(name)) continue;
            m_recovery.triedServers.insert(name);
            logWarn() << "Player" << label << "failed (" + m_recovery.reason + "); switching to" << name;
            resolveServer(i, m_recovery.at, m_recovery.fraction, false);
            return;
        }
    }

    m_recovering = false;
    logError() << "Player" << label << "could not be recovered:" << m_recovery.reason;
    if (m_loadingItem) {
        setLoadingItem({});
        setCurrentItem(item);
    }
    if (auto *mpv = MpvPlayer::instance(); mpv && mpv->progressing()) return;
    const double place = m_recovery.at > 0 ? m_recovery.at : -1;
    MainWindow::instance().reportError(
        place > 0 ? tr("%1 stopped: %2. Every server was tried; Reload (Ctrl+R) tries again from %3.")
                        .arg(label, m_recovery.reason,
                             QStringLiteral("%1:%2").arg(int(place) / 60).arg(int(place) % 60, 2, 10, QLatin1Char('0')))
                  : tr("%1 stopped: %2. Every server was tried; Reload (Ctrl+R) tries again.")
                        .arg(label, m_recovery.reason),
        tr("Playback"));
}

void Playlist::stop() {
    saveProgress(true);
    cancel();
    if (auto *mpv = MpvPlayer::instance()) mpv->stop();
    setCurrentItem({});
}

void Playlist::cancel() {
    if (m_appendFuture.isRunning()) m_appendCancel.cancel();
    if (m_linkChecks > 0) {
        m_linkCheck.cancel();
        m_queueCheck.cancel();
        m_queueCheck = CancelToken{};
    }
    if (m_watcher.isRunning()) {
        m_cancel.cancel();
    } else if (auto *mpv = MpvPlayer::instance()) {
        if (mpv->isLoading()) mpv->stop();
    }
}

void Playlist::cacheRemainingServers() {
    if (m_bgCacheFuture.isRunning()) { m_bgCachePending = true; return; }
    Provider *provider = m_serverListModel.provider();
    if (!provider) return;

    QList<VideoServer> toCheck;
    for (int i = 0; i < m_serverListModel.count(); ++i) {
        const auto &server = m_serverListModel.at(i);
        if (!m_serverListModel.cachedSource(server.name))
            toCheck.append(server);
    }
    if (toCheck.isEmpty()) return;

    // Fresh, not reset(): a reset would write the old episode's probes into the new list.
    m_bgCacheCancel = CancelToken{};
    m_bgCacheFuture = QtConcurrent::run([this, toCheck, provider, cancel = m_bgCacheCancel]() {
        for (const VideoServer &server : toCheck) {
            if (cancel.isCancelled()) return;
            auto verdict = ServerSelector::Playability::Unknown;
            Source source;
            try {
                Client client(cancel);
                source = provider->extractSource(&client, server);
                source.keepTrustedTracks();
                if (!cancel.isCancelled())
                    verdict = ServerSelector::playability(&client, source);
            } catch (const std::exception &e) {
                logWarn() << "Player" << server.name << "background cache failed:" << e.what();
            }
            if (cancel.isCancelled()) return;
            QMetaObject::invokeMethod(this, [this, name = server.name, source, verdict, cancel]() {
                if (cancel.isCancelled()) return;
                if (verdict == ServerSelector::Playability::Playable)
                    m_serverListModel.cacheSource(name, std::move(source));
                else if (verdict == ServerSelector::Playability::Broken)
                    m_serverListModel.markBroken(name);
            }, Qt::QueuedConnection);
        }
    });
    m_bgCacheWatcher.setFuture(m_bgCacheFuture);
}

void Playlist::appendShow(const QString &title, const QString &link, Provider *provider,
                                 QSharedPointer<PlaylistItem> cached, const ShowData::WatchState &watch, bool play) {
    if (m_appendFuture.isRunning()) return;
    if (!cached) cached = find(link);

    const bool replace = play && provider && provider->playsDirectly(link);
    auto commit = [this, replace](QSharedPointer<PlaylistItem> pl, const ShowData::WatchState &state, bool doPlay) {
        if (auto queued = find(pl->link); queued && queued != pl) {
            if (pl->currentIndex() >= 0) queued->setCurrentIndex(pl->currentIndex());
            pl = queued;
        }
        if (m_episodeResume) m_episodeResume(pl);
        if (state.lastWatchedIndex >= pl->count()) {
            MainWindow::instance().reportError(tr("That episode does not exist."), tr("Playback"));
            return;
        }
        if (state.lastWatchedIndex != -1) {
            pl->setCurrentIndex(state.lastWatchedIndex);
            if (auto item = pl->currentItem(); item && state.progress > 0) item->setProgress(state.progress);
        }
        int idx = append(pl);
        if (doPlay && replace) m_replacePending = pl;
        if (doPlay) playAt(idx);
    };

    if (cached) { commit(cached, watch, play); return; }

    m_appendCancel = CancelToken{};
    m_appendPlays = play;
    QTimer::singleShot(0, this, &Playlist::isLoadingChanged);
    m_appendFuture = QtConcurrent::run([this, title, link, provider, watch, play, commit,
                                        cancel = m_appendCancel]() {
        Client client(cancel);
        ShowData dummy(title, link, "", provider);
        QString failure;
        try {
            provider->loadShow(&client, dummy);
            if (!dummy.playlist() || dummy.playlist()->isEmpty()) failure = client.lastError();
        } catch (const std::exception &e) {
            failure = QString::fromUtf8(e.what());
        }
        if (cancel.isCancelled()) return;
        auto playlist = dummy.playlist();
        if (!playlist || playlist->isEmpty()) {
            const QString name = title.isEmpty() ? link : title;
            logWarn() << "Sources" << "could not load" << name << failure;
            MainWindow::instance().reportError(
                failure.isEmpty() ? tr("%1 has no episodes for %2.").arg(provider->name(), name)
                                  : tr("%1 could not load %2.\n\n%3").arg(provider->name(), name, failure),
                tr("Could not load show"));
            return;
        }
        QMetaObject::invokeMethod(this, [this, playlist, dummy, watch, play, commit, cancel]() {
            if (cancel.isCancelled()) return;
            emit metadataLoaded(dummy.link, dummy.title, dummy.coverUrl, dummy.provider->name(), playlist->episodeCount());
            commit(playlist, watch, play);
        }, Qt::QueuedConnection);
    });
    m_appendFuture.then(this, [this] { emit isLoadingChanged(); });
}

Playlist::Counted Playlist::countedAs(const QSharedPointer<PlaylistItem> &item) const {
    if (item && m_countedAs.itemLink == item->link) return m_countedAs;
    const auto list = item ? item->parent() : nullptr;
    return {item ? item->link : QString(), list ? list->link : QString(), item ? item->row() : -1,
            item ? item->number : -1.0f};
}

bool Playlist::playDownloaded(const QSharedPointer<PlaylistItem> &item, const QString &file) {
    if (m_watcher.isRunning()) {
        m_pendingItem = item;
        m_cancel.cancel();
        return false;
    }
    m_cancel = CancelToken{};
    m_pendingItem.clear();
    m_pendingServerIndex = -1;
    m_recovering = false;
    m_bgCacheCancel.cancel();
    if (const auto current = m_currentItem.toStrongRef(); current && current != item) saveProgress();
    m_serverListModel.clear();
    m_serversFor.clear();
    setLoadingItem(item);
    ensureMpvProgressConnection();
    Source source = localSource(file);
    source.progress = resumePoint(*item);
    logOk() << "Player" << "Playing the download of" << item->displayName.simplified();
    openResolved(source);
    return true;
}

void Playlist::queueDownloaded(const QString &path) {
    if (const KeptEpisode kept = m_keptLookup ? m_keptLookup(path) : KeptEpisode{}; kept.valid) {
        if (const auto show = find(kept.showLink)) {
            if (const int row = show->indexOf(kept.episodeLink); row >= 0 && canRemove(show.data())) {
                show->setCurrentIndex(row);
                refreshPath(show->at(row).data());
            }
            return;
        }
    }
    openUrl(QUrl::fromLocalFile(path), false);
}

QString Playlist::playingList() const {
    const auto current = m_currentItem.toStrongRef();
    const auto list = current ? current->parent() : nullptr;
    return list ? list->link : QString();
}

void Playlist::mergeEpisodes(const QSharedPointer<PlaylistItem> &list, const QSharedPointer<PlaylistItem> &fresh) {
    if (!list || !fresh || list == fresh) return;
    const bool inTree = find(list->link) == list;
    const QModelIndex parent = inTree ? indexFor(list.data()) : QModelIndex();

    QSet<QString> freshLinks;
    for (const auto &item : fresh->children()) freshLinks.insert(item->link);
    const auto current = m_currentItem.toStrongRef();
    int removed = 0, added = 0;
    for (int i = list->count() - 1; i >= 0; --i) {
        const auto item = list->at(i);
        if (item->isList() || freshLinks.contains(item->link) || item == current || item == m_loadingItem) continue;
        if (inTree) beginRemoveRows(parent, i, i);
        list->removeAt(i);
        if (inTree) endRemoveRows();
        ++removed;
    }

    const auto items = fresh->children();
    fresh->clear();
    for (int j = 0; j < items.size(); ++j) {
        if (list->indexOf(items[j]->link) >= 0) continue;
        const int at = qMin(j, list->count());
        if (inTree) beginInsertRows(parent, at, at);
        list->insert(at, items[j]);
        if (inTree) endInsertRows();
        ++added;
    }
    if (added || removed)
        logInfo() << "Library" << list->name << "gained" << added << "and lost" << removed << "episode(s)";
}

void Playlist::setCurrentItem(const QSharedPointer<PlaylistItem> &item) {
    const auto previous = m_currentItem.toStrongRef();
    if (!item || item->isList()) {
        m_currentItem = QWeakPointer<PlaylistItem>();
        refreshPath(previous.data());
        emit currentItemChanged(QModelIndex());
        if (item && item->isList())
            logWarn() << "Player" << "Cannot set current item to a list" << item->link;
        return;
    }
    m_currentItem = item;
    m_currentCompleted = false;
    m_lastProgressSaveMs = QDateTime::currentMSecsSinceEpoch();
    ensureMpvProgressConnection();

    if (auto *mpv = MpvPlayer::instance()) {
        auto p = item->parent();
        mpv->setShowKey(p ? p->link : item->link);
        mpv->setEpisodeKey(item->link);
    }

    int row = item->row();
    auto parent = item->parent();
    while (parent) {
        parent->setCurrentIndex(row);
        row = parent->row();
        parent = parent->parent();
    }
    if (previous != item) refreshPath(previous.data());
    refreshPath(item.data());
    emit currentItemChanged(indexFor(item->parent() && item->parent()->solo ? item->parent().data() : item.data()));
}

void Playlist::setLoadingItem(const QSharedPointer<PlaylistItem> &item) {
    if (m_loadingItem == item) return;
    const auto previous = std::exchange(m_loadingItem, item);
    refreshPath(previous.data());
    refreshPath(item.data());
}

void Playlist::refreshPath(const PlaylistItem *item) {
    for (auto list = item ? item->parent() : nullptr; list; list = list->parent()) {
        if (list->isEmpty()) continue;
        if (list->solo) {
            const QModelIndex row = indexFor(list.data());
            emit dataChanged(row, row, {IsCurrentIndexRole, IsDeletableRole, IsWatchedRole, ProgressRole});
            continue;
        }
        const QModelIndex parent = indexFor(list.data());
        emit dataChanged(index(0, 0, parent), index(list->count() - 1, 0, parent),
                         {IsCurrentIndexRole, IsDeletableRole, IsWatchedRole, ProgressRole});
    }
}

void Playlist::showCurrentItemName() const {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem) return;
    auto playlist = currentItem->parent();
    if (!playlist) return;

    QString path = playlist->name;
    auto current = playlist->parent();
    while (current && current != m_root) {
        path = current->name + " | " + path;
        current = current->parent();
    }
    QString displayText = QString("%1\n[%2/%3] %4\n%5")
                              .arg(path,
                                   QString::number(playlist->currentIndex() + 1),
                                   QString::number(playlist->episodeCount()),
                                   currentItem->displayName.simplified(),
                                   QDateTime::currentDateTime().toString("dd/MM/yyyy HH:mm:ss"));
    if (auto *mpv = MpvPlayer::instance()) mpv->showText(displayText);
}

void Playlist::markCurrentWatched() {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem || currentItem->preview || m_loadingItem) return;
    auto playlist = currentItem->parent();
    if (!playlist || !playlist->isList()) return;

    m_currentCompleted = true;
    currentItem->setProgress(1.0);
    saveProgress();
    const Counted counted = countedAs(currentItem);
    emit progressUpdated(counted.link, counted.row, 1.0);
    if (counted.number > 0)
        emit episodeCompleted(counted.link, int(counted.number));
    showCurrentItemName();
}

void Playlist::saveProgress(bool quiet) {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem || currentItem->preview || m_loadingItem) return;
    auto playlist = currentItem->parent();
    if (!playlist || !playlist->isList()) return;

    int row = currentItem->row();
    auto *mpv = MpvPlayer::instance();
    if (!mpv || mpv->state() == MpvPlayer::Stopped) return;

    const double duration = mpv->preciseDuration();
    const double progress = duration > 0 ? qBound(0.0, mpv->preciseTime() / duration, 1.0) : 0.0;
    if (!quiet)
        logInfo() << "Library" << playlist->name << "Saving | Index =" << row
               << "| Progress =" << QString::number(progress * 100, 'f', 1) + "%";

    currentItem->setProgress(progress);
    const QModelIndex shown = indexFor(playlist->solo ? playlist.data() : currentItem.data());
    emit dataChanged(shown, shown, {ProgressRole});
    if (playlist->isLocalDir())
        emit localProgressUpdated(currentItem->link, playlist->link, progress);
    const Counted counted = countedAs(currentItem);
    emit progressUpdated(counted.link, counted.row, progress);
    if (duration > 0) m_progressSync.push(currentItem, mpv->preciseTime(), duration);
}

void Playlist::ensureMpvProgressConnection() {
    if (m_mpvProgressConnected) return;
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    connect(mpv, &MpvPlayer::fileLoaded, this, [this]() {
        if (auto item = m_loadingItem) {
            setLoadingItem({});
            finalizePlayback(item);
        }
    });
    connect(mpv, &MpvPlayer::streamFailed, this, [this](const QString &reason, double at, quint64 generation) {
        if (generation != m_openedGeneration) {
            logInfo() << "Player" << "a failure of the previous file, ignored:" << reason;
            return;
        }
        recoverStream(reason, at);
    }, Qt::QueuedConnection);
    connect(mpv, &MpvPlayer::timeChanged,     this, &Playlist::onPlaybackProgress);
    connect(mpv, &MpvPlayer::durationChanged, this, &Playlist::onPlaybackProgress);

    connect(mpv, &MpvPlayer::mpvStateChanged, this, [this]() {
        auto *player = MpvPlayer::instance();
        if (!player || player->state() != MpvPlayer::Paused || player->buffering()) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastPauseSyncMs < kPauseSyncIntervalMs) return;
        m_lastPauseSyncMs = now;
        saveProgress(true);
    });
    m_mpvProgressConnected = true;
}

void Playlist::onPlaybackProgress() {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem || currentItem->preview || m_loadingItem) return;
    auto playlist = currentItem->parent();
    if (!playlist || !playlist->isList()) return;
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    const double duration = mpv->preciseDuration();
    if (duration <= 0) return;
    m_progressSync.reconcile(currentItem, mpv->preciseTime(), duration);
    // A prefetch fetched 4 s in can be twenty minutes old by the end, so it is asked again with three minutes to go.
    if (!m_prefetchRefreshed && duration - mpv->preciseTime() < 180) {
        m_prefetchRefreshed = true;
        if (m_prefetch.valid && QDateTime::currentMSecsSinceEpoch() - m_prefetch.resolvedAtMs > 10 * 60 * 1000) {
            m_prefetchCancel.cancel();
            m_prefetch = {};
            prefetchNextEpisode();
        }
    }
    const double progress = qBound(0.0, mpv->preciseTime() / duration, 1.0);
    const bool completed = progress >= Settings::instance().watchedFraction();

    const Counted counted = countedAs(currentItem);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastProgressSaveMs >= kProgressSaveIntervalMs) {
        m_lastProgressSaveMs = now;
        const bool crossed = completed && !m_currentCompleted;
        m_currentCompleted = completed;
        saveProgress(true);
        if (crossed && counted.number > 0)
            emit episodeCompleted(counted.link, int(counted.number));
        return;
    }

    if (completed == m_currentCompleted) return;
    m_currentCompleted = completed;
    currentItem->setProgress(progress);
    emit progressUpdated(counted.link, counted.row, progress);
    if (completed && counted.number > 0)
        emit episodeCompleted(counted.link, int(counted.number));
}

bool Playlist::tryPlay(const QSharedPointer<PlaylistItem> &item) {
    if (!item) return false;

    auto resolvedItem = item;

    auto parent = resolvedItem->isList() ? nullptr : resolvedItem->parent();
    auto owner = resolvedItem->isList() ? resolvedItem : parent;
    if (!owner) {
        logError() << "Player" << resolvedItem->link << "has no parent playlist";
        return false;
    }
    QString link = owner->link;
    auto playlist = find(link);
    if (!playlist) {
        logError() << "Player" << link << "is not registered";
        return false;
    }

    if (!resolvedItem->isList() && parent != playlist) {
        logWarn() << "Player" << "Item does not belong to registered playlist";
        int itemIndex = playlist->indexOf(resolvedItem->link);
        if (itemIndex != -1) {
            resolvedItem = playlist->at(itemIndex);
        } else {
            logError() << "Player" << "Item does not belong to registered playlist";
            return false;
        }
    }

    resolvedItem = resolveToPlayableItem(resolvedItem);
    if (!resolvedItem) return false;

    auto playlistRef = resolvedItem->parent();
    if (!playlistRef) return false;

    if ((resolvedItem->type & PlaylistItem::Online) && m_downloadLookup
        && std::exchange(m_streamInstead, {}) != resolvedItem->link) {
        if (const QString file = m_downloadLookup(playlistRef->link, resolvedItem->link); !file.isEmpty())
            return playDownloaded(resolvedItem, file);
    }

    if (tryUsePrefetch(resolvedItem)) return true;

    if (m_watcher.isRunning()) {
        m_pendingItem = resolvedItem;
        m_cancel.cancel();
        return false;
    }

    m_recovering = false;
    if (m_prefetchFuture.isRunning()) m_prefetchCancel.cancel();

    m_pendingItem.clear();
    m_pendingServerIndex = -1;
    m_cancel = CancelToken{};

    auto currentItem = m_currentItem.toStrongRef();
    if (currentItem && currentItem != resolvedItem)
        saveProgress();

    m_bgCacheCancel.cancel();

    setLoadingItem(resolvedItem);
    ensureMpvProgressConnection();
    m_watcher.setFuture(QtConcurrent::run([this, resolvedItem, playlistRef, cancel = m_cancel]() {
        return this->resolvePlayback(resolvedItem, playlistRef, cancel);
    }));
    emit isLoadingChanged();
    return true;
}

Source Playlist::resolvePlayback(const QSharedPointer<PlaylistItem> &item,
                                   const QSharedPointer<PlaylistItem> &playlist, const CancelToken &cancel) {
    if (!playlist || !playlist->isList()) {
        logError() << "Player" << item->name << "does not belong to any playlist!";
        return {};
    }

    Source source;
    try {
        source = loadSource(item, playlist, cancel);
    } catch (const AppException &) {
        if (item->availableAt > QDateTime::currentSecsSinceEpoch())
            throw AppException(tr("Higher membership tiers get %1 early. It opens to you on %2.")
                                   .arg(item->displayName.simplified(),
                                        QLocale().toString(QDateTime::fromSecsSinceEpoch(item->availableAt),
                                                           QStringLiteral("ddd d MMM, HH:mm"))),
                               tr("Early access"));
        throw;
    }
    if (source.videos.isEmpty() || cancel.isCancelled())
        return {};

    source.progress = resumePoint(*item);
    return source;
}

double Playlist::resumePoint(const PlaylistItem &item) {
    const double progress = item.progress();
    return progress >= Settings::instance().watchedFraction() ? 0.0 : progress;
}

QSharedPointer<PlaylistItem> Playlist::resolveToPlayableItem(QSharedPointer<PlaylistItem> item) {
    while (item && item->isList()) {
        if (item->isEmpty()) return {};

        auto currentItem = item->currentItem();
        if (currentItem) {
            item = currentItem;
            continue;
        }

        QSharedPointer<PlaylistItem> firstPlayable;
        for (const auto &child : item->children())
            if (!child->isList()) { firstPlayable = child; break; }
        item = firstPlayable ? firstPlayable : item->first();
    }
    return item;
}

Source Playlist::loadSource(const QSharedPointer<PlaylistItem> &item,
                                const QSharedPointer<PlaylistItem> &playlist, const CancelToken &cancel) {
    switch (item->type) {
    case PlaylistItem::Pasted: return loadPastedSource(item);
    case PlaylistItem::Online: return loadOnlineSource(item, playlist, cancel);
    case PlaylistItem::Local:  return loadLocalSource(item);
    default: return {};
    }
}

Source Playlist::loadPastedSource(const QSharedPointer<PlaylistItem> &item) {
    return pastedSource(item->link);
}

Source Playlist::pastedSource(const QString &link) {
    Source source;
    if (link.contains('|')) {
        QStringList parts = link.split('|');
        source.videos.emplaceBack(parts.takeFirst());
        for (const QString &headerLine : std::as_const(parts)) {
            QStringList keyValue = headerLine.split(": ", Qt::KeepEmptyParts);
            if (keyValue.size() == 2)
                source.headers.insert(keyValue[0].trimmed(), keyValue[1].trimmed());
        }
    } else {
        source.videos.emplaceBack(link);
    }
    return source;
}

namespace {

QString staleYtdlpHint() {
    const QString version = YtDlp::version();
    const QDate released = QDate::fromString(version.left(10), QStringLiteral("yyyy.MM.dd"));
    if (!released.isValid() || released.daysTo(QDate::currentDate()) <= 60) return {};
    return QStringLiteral("\n\n") + Playlist::tr("yt-dlp %1 is out of date. Update it in Settings > Player.").arg(version);
}

YtDlp::Verdict checkPastedLink(Source info, bool useYtdlp, const QString &runtime, const CancelToken &cancel) {
    const QUrl url = info.videos.first().url;
    Client client(cancel, false);
    client.setBypassEnabled(false).setMaxBodyBytes(64 * 1024).setTimeout(15000);
    auto headers = info.headers;
    headers.insert(QStringLiteral("Range"), QStringLiteral("bytes=0-2047"));
    const Client::Response response = client.get(url.toString(), headers);
    if (cancel.isCancelled()) return {};
    if (QString showLink; response.finalUrl.isValid() && response.finalUrl.host() != url.host()) {
        int episode = -1;
        if (ProviderList::forUrl(response.finalUrl, showLink, episode)) {
            YtDlp::Verdict verdict;
            verdict.providerUrl = response.finalUrl;
            return verdict;
        }
    }

    const QString type = response.header(QStringLiteral("Content-Type")).toLower();
    const QString start = response.body.left(512).trimmed().toLower();
    const bool page = type.contains(QLatin1String("html")) || start.startsWith(QLatin1String("<!doctype html"))
                      || start.startsWith(QLatin1String("<html"));
    if (response.code >= 200 && response.code < 400 && !page) {
        switch (ServerSelector::playability(&client, info)) {
        case ServerSelector::Playability::Playable: return {true, {}};
        case ServerSelector::Playability::Broken: return {false, Playlist::tr("The video at this link does not load.")};
        case ServerSelector::Playability::Unknown: return {false, Playlist::tr("The link timed out.")};
        }
    }
    if (useYtdlp) {
        YtDlp::Verdict verdict = YtDlp::check(url.toString(), info.headers,
                                              YtDlp::runtimeArguments(runtime), cancel);
        if (!verdict.playable && !cancel.isCancelled())
            verdict.error = Playlist::tr("yt-dlp cannot play this link: %1").arg(verdict.error) + staleYtdlpHint();
        return verdict;
    }
    if (page)
        return {false, Playlist::tr("This is a web page, not a video. Turn on Use yt-dlp in Settings > Player to play "
                                    "it.")};
    if (response.code <= 0) return {false, Playlist::tr("Could not reach %1.").arg(url.host())};
    return {false, Playlist::tr("The link answered HTTP %1.").arg(response.code)};
}

}

Source Playlist::loadOnlineSource(const QSharedPointer<PlaylistItem> &item,
                                      const QSharedPointer<PlaylistItem> &playlist, const CancelToken &cancel) {
    if (!playlist)
        throw AppException(tr("The playlist was closed while this episode loaded."), tr("Playlist"));
    auto provider = playlist->provider();
    if (!provider)
        throw AppException(tr("The playlist has no provider."), tr("Provider"));

    QString label = item->displayName;
    label.replace('\n', " - ");
    if (!playlist->name.isEmpty()) label = playlist->name + " " + label;
    label += " (" + provider->name() + ")";

    Client client(cancel);
    auto servers = provider->loadServers(&client, item.data());
    if (servers.isEmpty())
        throw AppException(tr("No servers found for %1.").arg(label), tr("Server"));

    std::sort(servers.begin(), servers.end(),
              [](const VideoServer &a, const VideoServer &b) {
                  return a.name < b.name;
              });

    auto result = ServerSelector::findWorkingServer(&client, provider, servers);
    if (!result.found()) {
        if (!result.failure.isEmpty())
            throw AppException(result.failure, result.failureHeader);
        throw AppException(tr("No working server found for %1 (tried %2).")
                               .arg(label).arg(servers.size()), tr("Server"));
    }

    if (cancel.isCancelled()) return {};

    const QString chosen = servers[result.index].name;
    QMetaObject::invokeMethod(this, [this, link = item->link, servers, provider, chosen, cancel,
                                     cache = ServerListModel::stamped(std::move(result.cachedSources))]() mutable {
        if (cancel.isCancelled()) return;
        applyServers(link, servers, provider, chosen, std::move(cache));
    }, Qt::QueuedConnection);

    return result.source;
}

void Playlist::applyServers(const QString &episodeLink, const QList<VideoServer> &servers,
                            Provider *provider, const QString &chosen, ServerListModel::SourceCache cache) {
    if (!m_serversFor.isEmpty() && m_serversFor != episodeLink && m_serverListModel.count() > 0)
        m_previous = Prefetch{true, QDateTime::currentMSecsSinceEpoch(), m_serversFor,
                              m_serverListModel.servers(), m_serverListModel.provider(),
                              m_serverListModel.currentName(), m_serverListModel.sources()};
    m_serversFor = episodeLink;
    m_serverListModel.setServers(servers, provider);
    m_serverListModel.setCachedSources(std::move(cache));
    m_serverListModel.setCurrentServer(chosen);
    cacheRemainingServers();
}

QSharedPointer<PlaylistItem> Playlist::nextItem() const {
    auto cur = m_currentItem.toStrongRef();
    if (!cur) return {};
    auto pl = cur->parent();
    if (!pl) return {};
    int ni = cur->row() + 1;
    if (ni < 0 || ni >= pl->count()) return {};
    return pl->at(ni);
}

void Playlist::prefetchNextEpisode() {
    auto next = nextItem();
    if (!next || next->isList() || next->type != PlaylistItem::Online) {
        m_prefetchTimer.stop();
        return;
    }
    if (m_prefetch.itemLink == next->link && m_prefetch.chosenSource()) return;
    m_prefetchCancel.cancel();
    m_prefetch = {};
    m_prefetchTimer.start();
}

void Playlist::startNextEpisodePrefetch() {
    // Assigning over a running future drops the handle the destructor waits on.
    if (m_watcher.isRunning() || m_prefetchFuture.isRunning()) {
        m_prefetchTimer.start();
        return;
    }
    auto next = nextItem();
    if (!next || next->isList() || next->type != PlaylistItem::Online) return;
    if (m_prefetch.itemLink == next->link && m_prefetch.chosenSource()) return;
    auto pl = next->parent();
    Provider *provider = pl ? pl->provider() : nullptr;
    if (!provider) return;

    m_prefetchCancel = CancelToken{};
    const QString link = next->link;
    const QString label = next->displayName.isEmpty() ? next->name : next->displayName;
    m_prefetchFuture = QtConcurrent::run([this, next, provider, link, label, cancel = m_prefetchCancel]() {
        if (cancel.isCancelled()) return;
        try {
            Client client(cancel);
            auto servers = provider->loadServers(&client, next.data());
            if (servers.isEmpty() || cancel.isCancelled()) return;
            std::sort(servers.begin(), servers.end(),
                      [](const VideoServer &a, const VideoServer &b) { return a.name < b.name; });
            auto result = ServerSelector::findWorkingServer(&client, provider, servers, ServerSelector::Background);
            if (!result.found() || cancel.isCancelled()) return;
            QMetaObject::invokeMethod(this, [this, link, label, servers, provider, cancel,
                                             chosen = servers[result.index].name,
                                             cache = ServerListModel::stamped(std::move(result.cachedSources))]() mutable {
                if (cancel.isCancelled()) return;
                m_prefetch = Prefetch{true, QDateTime::currentMSecsSinceEpoch(), link, servers, provider,
                                      chosen, std::move(cache)};
                logOk() << "Player" << "Prefetched the next episode:" << label;
            }, Qt::QueuedConnection);
        } catch (AppException &e) {
            logWarn() << "Player" << "Next-episode prefetch failed:" << e.what();
        } catch (const std::exception &e) {
            logWarn() << "Player" << "Next-episode prefetch failed:" << e.what();
        }
    });
}

bool Playlist::tryUsePrefetch(const QSharedPointer<PlaylistItem> &item) {
    if (item->type != PlaylistItem::Online || m_watcher.isRunning()) return false;
    const bool next = m_prefetch.itemLink == item->link && m_prefetch.chosenSource();
    const bool back = !next && m_previous.itemLink == item->link && m_previous.chosenSource();
    if (!next && !back) return false;

    Prefetch pf = std::exchange(next ? m_prefetch : m_previous, Prefetch{});
    Source source = *pf.chosenSource();
    m_cancel.cancel();
    if (next && !m_prefetchFuture.isRunning()) {
        m_cancel = m_prefetchCancel;
        m_prefetchCancel = CancelToken{};
    } else {
        m_cancel = CancelToken{};
    }
    m_prefetchTimer.stop();
    m_recovering = false;

    auto currentItem = m_currentItem.toStrongRef();
    if (currentItem && currentItem != item) saveProgress();

    m_pendingItem.clear();
    m_pendingServerIndex = -1;
    m_bgCacheCancel.cancel();

    applyServers(item->link, pf.servers, pf.provider, pf.chosen, std::move(pf.sources));
    setLoadingItem(item);
    ensureMpvProgressConnection();

    source.progress = resumePoint(*item);
    source.startSeconds = 0;
    logOk() << "Player" << "Using" << (next ? "prefetched" : "remembered") << "source for" << item->displayName;
    openResolved(source);
    return true;
}

Source Playlist::loadLocalSource(const QSharedPointer<PlaylistItem> &item) {
    if (!QFile::exists(item->link)) {
        logWarn() << "Player" << item->link << "does not exist";
        QMetaObject::invokeMethod(this, [this, item]() {
            auto playlist = item->parent();
            if (!playlist) return;
            int itemRow = item->row();
            if (itemRow < 0) return;
            bool wasCurrent = playlist->currentIndex() == itemRow;
            beginRemoveRows(indexFor(playlist.data()), itemRow, itemRow);
            playlist->removeAt(itemRow);
            endRemoveRows();
            if (wasCurrent) playlist->setCurrentIndex(-1);
        }, Qt::QueuedConnection);
        return {};
    }
    return localSource(item->link);
}

Source Playlist::localSource(const QString &path) {
    Source source;
    source.videos.emplaceBack(path);
    if (path.endsWith(QLatin1String(".mkv"), Qt::CaseInsensitive)) {
        const QFileInfo file(path);
        source.danmakuKey = QStringLiteral("local-%1").arg(
            QString::fromLatin1(QCryptographicHash::hash((file.absoluteFilePath() + QString::number(file.size())).toUtf8(),
                                                         QCryptographicHash::Md5).toHex().left(16)));
        source.danmakuSource = [path = file.absoluteFilePath(), ffmpeg = Settings::toolPath(QStringLiteral("ffmpeg.exe")),
                                  cache = Settings::tempDir() + QStringLiteral("/danmaku/kept"),
                                  started = std::make_shared<std::optional<QFuture<QList<DanmakuComment>>>>()]() {
            if (!*started)
                *started = QtConcurrent::run([path, ffmpeg, cache] { return DanmakuAss::fromVideoFile(path, ffmpeg, cache); });
            return **started;
        };
    }
    return source;
}

void Playlist::finalizePlayback(const QSharedPointer<PlaylistItem> &item) {
    {
        auto playlist = item->parent();
        if (!playlist) return;

        const int itemRow = item->row();
        playlist->setCurrentIndex(itemRow);
        m_countedAs = {item->link, playlist->link, itemRow, item->number};
        if (playlist->isLocalDir()) {
            if (const KeptEpisode kept = m_keptLookup ? m_keptLookup(item->link) : KeptEpisode{}; kept.valid) {
                int row = kept.episodeIndex;
                if (const auto show = find(kept.showLink))
                    if (const int at = show->indexOf(kept.episodeLink); at >= 0) row = at;
                if (row >= 0) {
                    m_countedAs = {item->link, kept.showLink, row, kept.number};
                    emit metadataLoaded(kept.showLink, kept.showTitle, kept.cover, kept.provider, 0);
                }
            } else {
                emit metadataLoaded(playlist->link, playlist->name, QString(), QString(), playlist->episodeCount());
            }
        }
        if (!item->preview) {
            if (playlist->isLocalDir())
                emit localProgressUpdated(item->link, playlist->link, item->progress());
            emit episodeStarted(m_countedAs.link, m_countedAs.row, item->name);
            emit progressUpdated(m_countedAs.link, m_countedAs.row, item->progress());
        }
        setCurrentItem(item);
        m_recovering = false;
        m_prefetchRefreshed = false;
        if (!(item->type & PlaylistItem::Online)) {
            m_serverListModel.clear();
            m_serversFor.clear();
        }
        if (const auto started = std::exchange(m_quickPlayPending, {}); started) {
            const auto queued = find(started->link);
            if (queued && queued->season == kQuickPlaySeason && isPlaying(queued->link))
                dropOtherQuickPlayRows(queued->link);
        }
        if (const auto started = std::exchange(m_replacePending, {}); started && isPlaying(started->link)) {
            const auto rows = m_root->children();
            for (const auto &row : rows)
                if (row != started && canRemove(row.data())) remove(indexFor(row.data()));
        }
        prefetchNextEpisode();
    }
}

void Playlist::openUrl(QUrl url, bool play) {
    LocalMedia::ParsedUrl parsed = LocalMedia::parse(url);

    if (!parsed.valid) {
        logError() << "Player" << "Invalid url:" << parsed.raw;
        MainWindow::instance().reportError(tr("Not a file, folder or web address:\n%1").arg(parsed.raw),
                                         tr("Cannot open"));
        return;
    }
    static QStringList subtitleExtensions = { "srt", "sub", "ssa", "ass", "idx", "vtt" };
    if (subtitleExtensions.contains(QFileInfo(parsed.url.path()).suffix()) ||
        parsed.url.path().toLower().contains("subtitle")) {
        if (auto *mpv = MpvPlayer::instance()) mpv->addSubtitle(Track(parsed.url));
        return;
    }

    if (parsed.url.isLocalFile()) {
        openLocalPath(parsed.url, parsed.raw, play);
        return;
    }

    QString showLink;
    int episodeIndex = -1;
    if (Provider *provider = ProviderList::forUrl(parsed.url, showLink, episodeIndex)) {
        logInfo() << "Player" << "Opening" << parsed.raw << "on" << provider->name();
        ShowData::WatchState watch;
        watch.lastWatchedIndex = episodeIndex;
        appendShow({}, showLink, provider, {}, watch, play);
        return;
    }
    openRemoteUrl(parsed.raw, parsed.url, play);
}

void Playlist::openLocalPath(const QUrl &url, const QString &urlString, bool play) {
    QFileInfo pathInfo(url.toLocalFile());
    QString dirPath = pathInfo.isDir() ? pathInfo.absoluteFilePath() : pathInfo.dir().absolutePath();
    logInfo() << "Player" << "Opening local file" << dirPath;

    auto playlist = find(dirPath);
    if (playlist) {
        if (const int row = pathInfo.isDir() ? -1 : playlist->indexOf(pathInfo.absoluteFilePath());
            row >= 0 && (play || canRemove(playlist.data()))) {
            playlist->setCurrentIndex(row);
            refreshPath(playlist->at(row).data());
        }
    } else {
        playlist = QSharedPointer<PlaylistItem>::create();
        if (LocalMedia::loadFolder(url, playlist, [this](const QString &p) { return m_byLink.contains(p); }, 0, 5)) {
            applyLocalResume(playlist);
            append(playlist);
            logInfo() << "Player" << "Loaded folder" << dirPath;
        } else {
            logInfo() << "Player" << "Failed to load folder" << dirPath;
            MainWindow::instance().reportError(tr("Nothing playable in %1.").arg(dirPath),
                                             tr("Cannot open"));
            playlist = nullptr;
        }
    }

    if (playlist && play) {
        if (auto *mpv = MpvPlayer::instance()) mpv->showText(tr("Playing: %1").arg(urlString));
        tryPlay(playlist);
    }
}

void Playlist::openRemoteUrl(const QString &urlString, const QUrl &url, bool play) {
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        addPastedUrl(urlString, url, play);
        return;
    }
    if (play) {
        m_linkCheck.cancel();
        m_linkCheck = CancelToken{};
    }
    const CancelToken cancel = play ? m_linkCheck : m_queueCheck;
    logInfo() << "Player" << "Checking" << urlString;
    if (auto *mpv = MpvPlayer::instance()) mpv->showText(tr("Checking %1…").arg(url.host()));
    const bool useYtdlp = Settings::instance().mpvYtdlEnabled();
    const QString runtime = Settings::instance().get(Config::YtdlpJsRuntime);

    ++m_linkChecks;
    emit isLoadingChanged();
    auto *watcher = new QFutureWatcher<YtDlp::Verdict>(this);
    connect(watcher, &QFutureWatcher<YtDlp::Verdict>::finished, this, [this, watcher, cancel, urlString, url, play] {
        watcher->deleteLater();
        --m_linkChecks;
        emit isLoadingChanged();
        if (cancel.isCancelled()) return;
        const YtDlp::Verdict verdict = watcher->result();
        if (verdict.providerUrl.isValid()) {
            logInfo() << "Player" << urlString << "leads to" << verdict.providerUrl.toString();
            openUrl(verdict.providerUrl, play);
        } else if (verdict.playable) {
            addPastedUrl(urlString, url, play);
        } else {
            logWarn() << "Player" << "Not playable:" << urlString << verdict.error;
            MainWindow::instance().reportError(verdict.error, tr("Cannot play link"));
        }
    });
    watcher->setFuture(QtConcurrent::run([info = pastedSource(urlString), useYtdlp, runtime, cancel] {
        return checkPastedLink(info, useYtdlp, runtime, cancel);
    }));
}

void Playlist::addPastedUrl(const QString &urlString, const QUrl &url, bool play) {
    logInfo() << "Player" << "Opening online video" << urlString;

    Provider *owner = ProviderList::ownerOfHost(url);
    const QString listLink = owner ? QStringLiteral("paste/") + owner->name()
                                   : QStringLiteral("videos");
    const QString listName = owner ? owner->name() : QStringLiteral("Videos");

    auto playlist = find(listLink);
    if (!playlist) {
        playlist = QSharedPointer<PlaylistItem>::create(listName, nullptr, listLink);
        append(playlist);
    }

    int itemIndex = playlist->indexOf(urlString);
    if (itemIndex == -1) {
        beginInsertRows(indexFor(playlist.data()), playlist->count(), playlist->count());
        playlist->emplaceBack(0, playlist->count() + 1, urlString, url.toString(), false);
        endInsertRows();
        playlist->last()->type = PlaylistItem::Pasted;
        itemIndex = playlist->count() - 1;
    }
    playlist->setCurrentIndex(itemIndex);

    if (play) {
        if (auto *mpv = MpvPlayer::instance()) mpv->showText(tr("Playing: %1").arg(urlString));
        tryPlay(playlist);
    }
}

void Playlist::onLocalDirectoryChanged(const QString &path) {
    m_changedFolders.insert(path);
    m_rescanTimer.start();
}

static bool isUnder(const QSharedPointer<PlaylistItem> &item, const QString &path) {
    for (auto node = item ? item->parent() : nullptr; node; node = node->parent())
        if (node->link == path) return true;
    return false;
}

void Playlist::rescanChangedFolders() {
    for (auto it = m_changedFolders.begin(); it != m_changedFolders.end();) {
        // Not while a file in it loads: the rebuild would free the item under the worker.
        if (isUnder(m_loadingItem, *it)) { ++it; continue; }
        const QString path = *it;
        it = m_changedFolders.erase(it);
        rescanFolder(path);
    }
    if (!m_changedFolders.isEmpty()) m_rescanTimer.start();
}

void Playlist::rescanFolder(const QString &path) {
    auto playlist = find(path);
    if (!playlist) return;
    auto currentItem = m_currentItem.toStrongRef();
    const bool holdsCurrent = isUnder(currentItem, path);
    const QString prevLink = holdsCurrent ? currentItem->link : QString();

    if (holdsCurrent) saveProgress(true);

    logInfo() << "Player" << "Directory" << path << "has changed";
    deregisterPlaylist(playlist);
    beginResetModel();
    const bool loaded = LocalMedia::loadFolder(QUrl::fromLocalFile(path), playlist,
                                                [this](const QString &p) { return !find(p).isNull(); }, 0, 5);
    if (loaded) applyLocalResume(playlist);
    endResetModel();
    if (loaded) {
        registerPlaylist(playlist);
        if (holdsCurrent) {
            QSharedPointer<PlaylistItem> again;
            visitListNodes(playlist, [&](const QSharedPointer<PlaylistItem> &node) {
                if (const int i = again ? -1 : node->indexOf(prevLink); i >= 0 && !node->at(i)->isList())
                    again = node->at(i);
            });
            if (again) {
                setCurrentItem(again);
            } else {
                setCurrentItem(playlist->currentItem());
                tryPlay(playlist);
            }
        } else {
            setCurrentItem(m_currentItem.toStrongRef());
        }
        return;
    }

    logInfo() << "Player" << "Failed to reload folder" << playlist->link;
    if (auto *mpv = MpvPlayer::instance(); mpv && holdsCurrent) mpv->pause();
    auto parent = playlist->parent();
    if (!parent) return;
    int plRow = playlist->row();
    beginRemoveRows(indexFor(parent.data()), plRow, plRow);
    parent->removeOne(playlist);
    endRemoveRows();
    setCurrentItem(m_currentItem.toStrongRef());
}

void Playlist::applyLocalResume(const QSharedPointer<PlaylistItem> &playlist) {
    if (!m_localResume || !playlist) return;
    visitListNodes(playlist, [this](const QSharedPointer<PlaylistItem> &node) {
        if (!node->isLocalDir()) return;
        const auto rows = m_localResume(node->link);
        if (rows.isEmpty()) return;

        QHash<QString, int> indexByPath;
        const auto &children = node->children();
        for (int i = 0; i < children.size(); ++i)
            if (!children[i]->isList()) indexByPath.insert(children[i]->link, i);
        if (indexByPath.isEmpty()) return;

        QStringList stale;
        int resumeIndex = -1;
        for (const auto &[path, progress] : rows) {
            const auto it = indexByPath.constFind(path);
            if (it == indexByPath.constEnd()) { stale << path; continue; }
            children[*it]->setProgress(progress);
            if (resumeIndex < 0) resumeIndex = *it;
        }

        if (node->currentIndex() == -1 && resumeIndex >= 0) node->setCurrentIndex(resumeIndex);
        if (!stale.isEmpty()) emit localProgressStale(stale);
    });
}

void Playlist::visitListNodes(const QSharedPointer<PlaylistItem> &root, const PlaylistVisitor &visitor) {
    QList<QSharedPointer<PlaylistItem>> queue{root};
    for (int front = 0; front < queue.size(); ++front) {
        const auto item = queue.at(front);
        visitor(item);
        for (const auto &child : item->children())
            if (child->isList())
                queue.append(child);
    }
}

void Playlist::registerPlaylist(const QSharedPointer<PlaylistItem> &playlist) {
    if (!playlist || !playlist->isList() || find(playlist->link)) return;
    visitListNodes(playlist, [this](const QSharedPointer<PlaylistItem> &item) {
        m_byLink.insert(item->link, QWeakPointer<PlaylistItem>(item));
        if (item->isLocalDir())
            m_folderWatcher.addPath(item->link);
    });
}

void Playlist::deregisterPlaylist(const QSharedPointer<PlaylistItem> &playlist) {
    if (!playlist || !m_byLink.contains(playlist->link)) return;
    visitListNodes(playlist, [this](const QSharedPointer<PlaylistItem> &item) {
        m_byLink.remove(item->link);
        if (item->isLocalDir())
            m_folderWatcher.removePath(item->link);
    });
}

QModelIndex Playlist::currentChild(const QModelIndex &idx) const {
    auto currentPlaylist = static_cast<PlaylistItem*>(idx.internalPointer());
    if (!currentPlaylist) return QModelIndex();
    if (currentPlaylist->solo) return idx;
    int index = currentPlaylist->currentIndex();
    if (!currentPlaylist->isValidIndex(index)) index = 0;
    if (!currentPlaylist->isValidIndex(index)) return QModelIndex();
    return createIndex(index, 0, currentPlaylist->at(index).data());
}

QString Playlist::currentShowName() const {
    auto cur = m_currentItem.toStrongRef();
    auto parent = cur ? cur->parent() : nullptr;
    return parent ? parent->name : QString();
}

QString Playlist::currentItemName() const {
    auto cur = m_currentItem.toStrongRef();
    return cur ? cur->displayName : QString();
}

QString Playlist::nextItemName() const {
    const auto current = m_currentItem.toStrongRef();
    const auto list = current ? current->parent() : nullptr;
    if (!list) return {};
    if (const auto next = list->at(current->row() + 1)) return next->displayName;
    const auto parent = list->parent();
    const auto nextList = parent ? parent->at(list->row() + 1) : nullptr;
    if (!nextList) return {};
    return nextList->name;
}

int Playlist::currentShowEpisodeCount() const {
    auto cur = m_currentItem.toStrongRef();
    auto parent = cur ? cur->parent() : nullptr;
    return parent ? parent->episodeCount() : 0;
}

static PlaylistItem *providerShowOf(PlaylistItem *item) {
    for (PlaylistItem *p = item; p; p = p->parent().data())
        if (p->isList() && p->provider()) return p;
    return nullptr;
}

QString Playlist::showLinkAt(const QModelIndex &index) const {
    auto *item = index.isValid() ? static_cast<PlaylistItem *>(index.internalPointer()) : nullptr;
    PlaylistItem *show = item ? providerShowOf(item) : nullptr;
    return show ? show->link : QString();
}

QString Playlist::showProviderAt(const QModelIndex &index) const {
    auto *item = index.isValid() ? static_cast<PlaylistItem *>(index.internalPointer()) : nullptr;
    PlaylistItem *show = item ? providerShowOf(item) : nullptr;
    return show && show->provider() ? show->provider()->name() : QString();
}

QString Playlist::showTitleAt(const QModelIndex &index) const {
    auto *item = index.isValid() ? static_cast<PlaylistItem *>(index.internalPointer()) : nullptr;
    PlaylistItem *show = item ? providerShowOf(item) : nullptr;
    return show ? show->name : QString();
}

QString Playlist::currentShowLink() const {
    auto cur = m_currentItem.toStrongRef();
    PlaylistItem *show = cur ? providerShowOf(cur.data()) : nullptr;
    return show ? show->link : QString();
}

QVariantMap Playlist::subtitleHints() const {
    auto cur = m_currentItem.toStrongRef();
    PlaylistItem *show = cur ? providerShowOf(cur.data()) : nullptr;
    if (!show) return {};
    const QString tmdb = show->provider() ? show->provider()->tmdbRef(show->link) : QString();
    const bool single = tmdb.startsWith(QLatin1String("movie/")) || show->episodeCount() <= 1;
    const bool whole = cur->number >= 0 && cur->number == std::floor(cur->number);
    return {{"show", show->name}, {"title", cur->name}, {"tmdb", tmdb},
            {"season", single ? 0 : cur->season},
            {"episode", single || !whole ? 0 : int(cur->number)}};
}

QString Playlist::currentShowProvider() const {
    auto cur = m_currentItem.toStrongRef();
    PlaylistItem *show = cur ? providerShowOf(cur.data()) : nullptr;
    return show && show->provider() ? show->provider()->name() : QString();
}

int Playlist::rowCount(const QModelIndex &parent) const {
    if (parent.column() > 0) return 0;
    PlaylistItem *parentItem = parent.isValid()
                                   ? static_cast<PlaylistItem*>(parent.internalPointer())
                                   : m_root.data();
    return parentItem->solo ? 0 : parentItem->count();
}

QModelIndex Playlist::index(int row, int column, const QModelIndex &parent) const {
    if (!hasIndex(row, column, parent)) return QModelIndex();
    PlaylistItem *parentItem = parent.isValid()
                                   ? static_cast<PlaylistItem*>(parent.internalPointer())
                                   : m_root.data();
    auto childItem = parentItem->at(row).data();
    return childItem ? createIndex(row, column, childItem) : QModelIndex();
}

QModelIndex Playlist::parent(const QModelIndex &childIndex) const {
    if (!childIndex.isValid()) return QModelIndex();
    auto *childItem = static_cast<PlaylistItem*>(childIndex.internalPointer());
    auto parentItem = childItem ? childItem->parent() : nullptr;
    if (!parentItem || parentItem.get() == m_root.data())
        return QModelIndex();
    return createIndex(parentItem->row(), 0, parentItem.data());
}

QVariant Playlist::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) return QVariant();
    auto *item = static_cast<PlaylistItem*>(index.internalPointer());
    if (!item) return QVariant();

    switch (role) {
    case Qt::DisplayRole:
        return item->isList() ? item->name : item->displayName;
    case TitleRole:
        return item->name;
    case IndexRole:
        return index;
    case NumberRole:
        return item->number;
    case IsCurrentIndexRole: {
        if (item->solo) return m_currentItem.toStrongRef() == item->at(0);
        auto parent = item->parent();
        if (!parent || parent->currentIndex() == -1) return false;
        return parent->currentIndex() == item->row();
    }
    case IsDeletableRole:
        return canRemove(item);
    case IsLocalRole:
        return item->isLocalDir();
    case ProgressRole:
        return item->solo && item->at(0) ? item->at(0)->progress() : item->progress();
    case LinkRole:
        return item->link;
    case IsWatchedRole: {
        if (item->isList()) return false;
        auto parent = item->parent();
        return parent && parent->currentIndex() > item->row();
    }
    default:
        return QVariant();
    }
}

bool Playlist::isFilteredOut(const QModelIndex &index, const QString &filter) const {
    if (filter.isEmpty() || !index.isValid()) return false;
    auto *item = static_cast<PlaylistItem*>(index.internalPointer());
    if (!item || item->isList()) return false;
    const QString f = filter.toLower();
    if (item->displayName.toLower().contains(f)) return false;
    if (QString::number(item->number).contains(filter)) return false;
    return true;
}

QHash<int, QByteArray> Playlist::roleNames() const {
    return {
        {TitleRole, "title"},
        {NumberRole, "number"},
        {IndexRole, "index"},
        {Qt::DisplayRole, "display"},
        {IsCurrentIndexRole, "isCurrentIndex"},
        {IsDeletableRole, "isDeletable"},
        {LinkRole, "link"},
        {IsWatchedRole, "isWatched"},
        {IsLocalRole, "isLocal"},
        {ProgressRole, "progress"},
    };
}
void Playlist::openResolved(Source &info) {
    if (auto *mpv = MpvPlayer::instance()) {
        if (m_loadingItem) {
            auto parent = m_loadingItem->parent();
            mpv->setShowKey(parent ? parent->link : m_loadingItem->link);
            mpv->setEpisodeKey(m_loadingItem->link);
        }
        if (m_resumeAt > 0) {
            info.startSeconds = m_resumeAt;
            info.progress = 0;
        } else if (m_resumeFraction >= 0) {
            info.startSeconds = 0;
            info.progress = m_resumeFraction;
        }
        m_resumeAt = m_resumeFraction = -1;
        mpv->open(info);
        m_openedInfo = info;
        m_openedGeneration = mpv->openGeneration();
    }
}
