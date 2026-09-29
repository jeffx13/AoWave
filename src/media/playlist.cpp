#include "media/playlist.h"
#include <cmath>
#include "core/async.h"
#include "core/logger.h"
#include "core/exception.h"
#include "media/mpvplayer.h"
#include "shows/showprovider.h"
#include "shows/providerlist.h"
#include "core/appshell.h"
#include "media/serverselector.h"
#include "net/cloudflare.h"
#include "media/localmedia.h"
#include "core/settings.h"
#include "media/ytdlp.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QtConcurrent/QtConcurrentTask>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <algorithm>
#include <QMetaObject>

Playlist::Playlist(QObject *parent) : QAbstractItemModel(parent) {
    registerPlaylist(m_root);
    connect(&m_folderWatcher, &QFileSystemWatcher::directoryChanged, this, &Playlist::onLocalDirectoryChanged);
    connect(&m_watcher, &QFutureWatcher<PlayInfo>::finished, this, &Playlist::onPlayFinished);
    // After onPlayFinished, so isLoading reads the post-handoff state.
    connect(&m_watcher, &QFutureWatcher<PlayInfo>::started,  this, &Playlist::isLoadingChanged);
    connect(&m_watcher, &QFutureWatcher<PlayInfo>::finished, this, &Playlist::isLoadingChanged);

    m_prefetchTimer.setSingleShot(true);
    m_prefetchTimer.setInterval(4000);   // let the current episode buffer first
    connect(&m_prefetchTimer, &QTimer::timeout, this, &Playlist::startNextEpisodePrefetch);
    connect(&m_progressSync, &ProgressSync::remoteResumeResolved, this, &Playlist::remoteResumeResolved);
}

void Playlist::onPlayFinished() {
    bool handedToPlayer = false;
    if (!m_cancel.isCancelled()) {
        try {
            auto playItem = m_watcher.result();
            if (!playItem.videos.isEmpty()) {
                openResolved(playItem);
                handedToPlayer = true;
            }
        } catch (AppException &ex) {
            ex.report();
        } catch (const std::exception &ex) {
            AppShell::instance().reportError(ex.what(), tr("Playlist error"));
        } catch (...) {
            AppShell::instance().reportError(tr("Something went wrong"), tr("Playlist error"));
        }
    }
    if (!handedToPlayer) m_loadingItem.clear();

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
    disconnect(&m_watcher, &QFutureWatcher<PlayInfo>::finished, this, nullptr);
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

    auto currentItem = m_currentItem.toStrongRef();
    setCurrentItem(currentItem);
    return index;
}

bool Playlist::isPlaying(const QString &link) const {
    if (link.isEmpty()) return false;
    for (auto item = m_currentItem.toStrongRef(); item; item = item->parent())
        if (item->link == link) return true;
    return false;
}

// A migration rewrites only the library row, so later saves would file under a dead link.
bool Playlist::relink(const QString &oldLink, const QString &newLink) {
    if (oldLink.isEmpty() || newLink.isEmpty() || oldLink == newLink) return false;
    auto playlist = find(oldLink);
    if (!playlist) return false;
    if (m_byLink.contains(newLink)) {
        logWarn() << "Playlist" << newLink << "is already open; leaving" << oldLink << "as it is";
        return false;
    }
    m_byLink.remove(oldLink);
    playlist->link = newLink;
    m_byLink.insert(newLink, playlist.toWeakRef());
    logInfo() << "Playlist" << "relinked" << oldLink << "->" << newLink;
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
        logError() << "Playlist" << "Invalid index:" << index << "to replace";
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

void Playlist::remove(const QModelIndex &index) {
    auto item = static_cast<PlaylistItem*>(index.internalPointer());
    if (!item) return;
    auto parent = item->parent();
    int row = index.row();
    if (!parent || (parent->currentIndex() != -1 && parent->currentItem().data() == item)) return;

    if (item->isList())
        deregisterPlaylist(item->sharedFromThis());

    beginRemoveRows(indexFor(parent.data()), row, row);
    parent->removeAt(row);
    endRemoveRows();

    if (parent->isEmpty()) {
        auto grandparent = parent->parent();
        if (grandparent) {
            int parentRow = parent->row();
            beginRemoveRows(indexFor(grandparent.data()), parentRow, parentRow);
            grandparent->removeAt(parentRow);
            endRemoveRows();
        }
    }

    auto currentItem = m_currentItem.toStrongRef();
    setCurrentItem(currentItem);
}

void Playlist::clear() {
    auto currentPlaylist = m_root->currentItem();

    for (const auto &playlist : m_root->children())
        if (playlist != currentPlaylist)
            deregisterPlaylist(playlist);
    // clear() frees children still inside live QModelIndexes.
    beginResetModel();
    m_root->clear();
    if (currentPlaylist)
        m_root->append(currentPlaylist);
    endResetModel();
    m_root->setCurrentIndex(currentPlaylist ? 0 : -1);

    auto currentItem = m_currentItem.toStrongRef();
    setCurrentItem(currentItem);
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

// remove() refuses the playing row, so a failed switch keeps its place.
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
        logError() << "Playlist" << currentItem->link << "does not belong to a playlist";
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
    if (!item || item->isList()) return;
    auto playlist = item->parent();
    if (playlist) tryPlay(playlist->at(item->row()));
}

void Playlist::reload() {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem) return;
    if (auto *mpv = MpvPlayer::instance(); mpv && mpv->duration() > 0)
        currentItem->setProgress(double(mpv->time()) / double(mpv->duration()));
    tryPlay(currentItem);
}

void Playlist::loadServer(int index) {
    if (m_watcher.isRunning()) {
        m_pendingServerIndex = index;
        m_cancel.cancel();
        return;
    }
    if (!m_serverListModel.isValidIndex(index)) return;
    VideoServer server = m_serverListModel.at(index);
    ShowProvider *provider = m_serverListModel.provider();
    if (!provider) return;

    logInfo() << "Server" << "Switching to" << server.name;

    if (const auto *cached = m_serverListModel.cachedSource(server.name)) {
        PlayInfo playItem = *cached;
        if (auto *mpv = MpvPlayer::instance()) {
            // The cached entry skips the probe, so the clearance headers may be stale.
            if (!playItem.videos.isEmpty())
                Cloudflare::applyClearanceHeaders(playItem.videos.first().url, playItem.headers);
            if (mpv->duration() > 0)
                playItem.progress = double(mpv->time()) / double(mpv->duration());
            mpv->open(playItem);
        }
        m_serverListModel.setCurrentIndex(index);
        m_serverListModel.setPreferredServer(index);
        logInfo() << "Server" << "Loaded" << server.name << "(cached)";
        return;
    }

    m_cancel = CancelToken{};
    m_watcher.setFuture(QtConcurrent::run([this, server, provider, cancel = m_cancel]() {
        Client client(cancel);
        PlayInfo playItem = provider->extractSource(&client, server);
        const auto verdict = ServerSelector::playability(&client, playItem);
        if (verdict != ServerSelector::Playability::Playable && !client.isCancelled()) {
            playItem.clear();
            if (verdict == ServerSelector::Playability::Broken) {
                logWarn() << "Server" << server.name << "is broken";
                QMetaObject::invokeMethod(this, [this, name = server.name, cancel]() {
                    if (!cancel.isCancelled()) m_serverListModel.markBroken(name);
                }, Qt::QueuedConnection);
            } else {
                logWarn() << "Server" << server.name << "did not answer - left unchecked";
            }
        }
        if (playItem.videos.isEmpty()) {
            logWarn() << "Server" << QString("Failed to load server %1").arg(server.name);
            return playItem;
        }
        if (auto *mpv = MpvPlayer::instance(); mpv && mpv->duration() > 0)
            playItem.progress = double(mpv->time()) / double(mpv->duration());
        QMetaObject::invokeMethod(this, [this, serverName = server.name, playItem, cancel]() {
            if (cancel.isCancelled()) return;
            // cacheSource resorts when a broken server recovers.
            m_serverListModel.cacheSource(serverName, playItem);
            m_serverListModel.setCurrentServer(serverName);
            logInfo() << "Server" << "Loaded" << serverName;
        }, Qt::QueuedConnection);
        return playItem;
    }));
    emit isLoadingChanged();   // watcher started arrives a turn later
}

void Playlist::tryNextServer() {
    if (m_watcher.isRunning()) return;
    if (m_loadingItem) {
        m_loadingItem.clear();
        return;
    }
    // Signed urls expire mid-episode, so the stream is re-resolved once.
    auto currentItem = m_currentItem.toStrongRef();
    if (currentItem && (currentItem->type & PlaylistItem::Online) && m_serverListModel.count() <= 1
        && m_streamRecoveryAttempts < 1) {
        ++m_streamRecoveryAttempts;
        if (auto *mpv = MpvPlayer::instance())
            m_recoveryProgress = mpv->preciseDuration() > 0 ? mpv->preciseTime() / mpv->preciseDuration() : currentItem->progress();
        logWarn() << "Playlist" << "Stream failed; resolving it again once from" << currentItem->displayName;
        m_serverListModel.clear();
        tryPlay(currentItem);
        return;
    }
    const int current = m_serverListModel.currentIndex();
    if (m_serverListModel.isValidIndex(current))
        m_autoTriedServers.insert(m_serverListModel.at(current).name);

    for (int i = 0; i < m_serverListModel.count(); ++i) {
        if (i == current) continue;
        const QString name = m_serverListModel.at(i).name;
        if (m_autoTriedServers.contains(name)) continue;
        if (!m_serverListModel.cachedSource(name)) continue;
        logOk() << "Server" << "Playback failed - auto-switching to" << name;
        loadServer(i);
        return;
    }
    logError() << "Server" << "Playback failed and no other working server is available";
}

void Playlist::stop() {
    saveProgress(true);
    cancel();
    if (auto *mpv = MpvPlayer::instance()) mpv->stop();
    setCurrentItem({});
}

void Playlist::cancel() {
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
    if (m_bgCacheFuture.isRunning()) return;
    ShowProvider *provider = m_serverListModel.provider();
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
        QList<QFuture<void>> jobs;
        jobs.reserve(toCheck.size());
        for (const VideoServer &server : toCheck) {
            jobs.push_back(QtConcurrent::task([this, server, provider, cancel]() {
                if (cancel.isCancelled()) return;
                auto verdict = ServerSelector::Playability::Unknown;
                PlayInfo playInfo;
                try {
                    Client client(cancel);
                    playInfo = provider->extractSource(&client, server);
                    if (!cancel.isCancelled())
                        verdict = ServerSelector::playability(&client, playInfo);
                } catch (const std::exception &e) {
                    logWarn() << "Server" << server.name << "background cache failed:" << e.what();
                }
                if (cancel.isCancelled()) return;
                QMetaObject::invokeMethod(this, [this, name = server.name, playInfo, verdict, cancel]() {
                    if (cancel.isCancelled()) return;
                    if (verdict == ServerSelector::Playability::Playable)
                        m_serverListModel.cacheSource(name, std::move(playInfo));
                    else if (verdict == ServerSelector::Playability::Broken)
                        m_serverListModel.markBroken(name);
                }, Qt::QueuedConnection);
            }).onThreadPool(ServerSelector::probePool()).withPriority(ServerSelector::Background).spawn());
        }
        for (auto &j : jobs) j.waitForFinished();
    });
}

void Playlist::appendShow(const QString &title, const QString &link, ShowProvider *provider,
                                 QSharedPointer<PlaylistItem> cached, const ShowData::WatchState &watch, bool play) {
    if (m_appendFuture.isRunning()) return;
    if (!cached) cached = find(link);

    auto commit = [this](QSharedPointer<PlaylistItem> pl, const ShowData::WatchState &state, bool doPlay) {
        if (auto queued = find(pl->link); queued && queued != pl) {
            if (pl->currentIndex() >= 0) queued->setCurrentIndex(pl->currentIndex());
            pl = queued;
        }
        if (m_episodeResume) m_episodeResume(pl);
        if (state.lastWatchedIndex >= pl->count()) {
            AppShell::instance().reportError(tr("The requested part does not exist."), tr("Playback"));
            return;
        }
        if (state.lastWatchedIndex != -1) {
            pl->setCurrentIndex(state.lastWatchedIndex);
            if (auto item = pl->currentItem(); item && state.progress > 0) item->setProgress(state.progress);
        }
        int idx = append(pl);
        if (doPlay) playAt(idx);
    };

    if (cached) { commit(cached, watch, play); return; }

    m_appendCancel = CancelToken{};
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
            logWarn() << "Playlist" << "could not load" << name << failure;
            AppShell::instance().reportError(
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
}

void Playlist::setCurrentItem(const QSharedPointer<PlaylistItem> &item) {
    if (!item || item->isList()) {
        m_currentItem = QWeakPointer<PlaylistItem>();
        emit currentItemChanged(QModelIndex());
        if (item && item->isList())
            logWarn() << "Playlist" << "Cannot set current item to a list" << item->link;
        return;
    }
    m_currentItem = item;
    m_currentCompleted = false;
    // finalizePlayback just stamped this item.
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
    emit currentItemChanged(indexFor(m_currentItem.toStrongRef().data()));
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

// Through the signal, so trackers, library and sidebar all hear it one way.
void Playlist::markCurrentWatched() {
    auto currentItem = m_currentItem.toStrongRef();
    if (!currentItem || currentItem->preview || m_loadingItem) return;
    auto playlist = currentItem->parent();
    if (!playlist || !playlist->isList()) return;

    m_currentCompleted = true;
    currentItem->setProgress(1.0);
    saveProgress();
    emit progressUpdated(playlist->link, currentItem->row(), 1.0);
    if (currentItem->number > 0)
        emit episodeCompleted(playlist->link, int(currentItem->number));
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
        logInfo() << "Playlist" << playlist->name << "Saving | Index =" << row
               << "| Progress =" << QString::number(progress * 100, 'f', 1) + "%";

    currentItem->setProgress(progress);
    if (playlist->isLocalDir())
        emit localProgressUpdated(currentItem->link, playlist->link, progress);
    emit progressUpdated(playlist->link, row, progress);
    if (duration > 0) m_progressSync.push(currentItem, mpv->preciseTime(), duration);
}

void Playlist::ensureMpvProgressConnection() {
    if (m_mpvProgressConnected) return;
    auto *mpv = MpvPlayer::instance();
    if (!mpv) return;
    connect(mpv, &MpvPlayer::fileLoaded, this, [this]() {
        if (auto item = std::exchange(m_loadingItem, {})) finalizePlayback(item);
    });
    connect(mpv, &MpvPlayer::timeChanged,     this, &Playlist::onPlaybackProgress);
    connect(mpv, &MpvPlayer::durationChanged, this, &Playlist::onPlaybackProgress);

    // A buffering stall is excluded: the position has not moved.
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
    const double progress = qBound(0.0, mpv->preciseTime() / duration, 1.0);
    const bool completed = progress >= Settings::instance().watchedFraction();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastProgressSaveMs >= kProgressSaveIntervalMs) {
        m_lastProgressSaveMs = now;
        const bool crossed = completed && !m_currentCompleted;
        m_currentCompleted = completed;
        saveProgress(true);
        // The periodic save usually wins the race to the mark.
        if (crossed && currentItem->number > 0)
            emit episodeCompleted(playlist->link, int(currentItem->number));
        return;
    }

    if (completed == m_currentCompleted) return;
    m_currentCompleted = completed;
    currentItem->setProgress(progress);
    emit progressUpdated(playlist->link, currentItem->row(), progress);
    if (completed && currentItem->number > 0)
        emit episodeCompleted(playlist->link, int(currentItem->number));
}

bool Playlist::tryPlay(const QSharedPointer<PlaylistItem> &item) {
    if (!item) return false;

    auto resolvedItem = item;

    auto parent = resolvedItem->isList() ? nullptr : resolvedItem->parent();
    auto owner = resolvedItem->isList() ? resolvedItem : parent;
    if (!owner) {
        logError() << "Playlist" << resolvedItem->link << "has no parent playlist";
        return false;
    }
    QString link = owner->link;
    auto playlist = find(link);
    if (!playlist) {
        logError() << "Playlist" << link << "is not registered";
        return false;
    }

    if (!resolvedItem->isList() && parent != playlist) {
        logWarn() << "Playlist" << "Item does not belong to registered playlist";
        int itemIndex = playlist->indexOf(resolvedItem->link);
        if (itemIndex != -1) {
            resolvedItem = playlist->at(itemIndex);
        } else {
            logError() << "Item does not belong to registered playlist";
            return false;
        }
    }

    // The worker must not race tree mutation.
    resolvedItem = resolveToPlayableItem(resolvedItem);
    if (!resolvedItem) return false;

    auto playlistRef = resolvedItem->parent();
    if (!playlistRef) return false;

    if (tryUsePrefetch(resolvedItem)) return true;

    if (m_watcher.isRunning()) {
        m_pendingItem = resolvedItem;
        m_cancel.cancel();
        return false;
    }

    // A different episode than any prefetch.
    m_prefetchCancel.cancel();
    m_prefetch = {};

    m_pendingItem.clear();
    m_pendingServerIndex = -1;
    // Fresh, not reset(): a reset would un-cancel a worker still winding down.
    m_cancel = CancelToken{};

    auto currentItem = m_currentItem.toStrongRef();
    if (currentItem && currentItem != resolvedItem)
        saveProgress();

    // The list stays the playing episode's until the new one starts.
    m_bgCacheCancel.cancel();

    if (currentItem != resolvedItem) m_streamRecoveryAttempts = 0;
    m_loadingItem = resolvedItem;
    ensureMpvProgressConnection();
    m_watcher.setFuture(QtConcurrent::run([this, resolvedItem, playlistRef, cancel = m_cancel]() {
        return this->resolvePlayback(resolvedItem, cancel);
    }));
    emit isLoadingChanged();   // watcher started arrives a turn later
    return true;
}

PlayInfo Playlist::resolvePlayback(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel) {
    auto playlist = item->parent();
    if (!playlist || !playlist->isList()) {
        logError() << "Playlist" << item->name << "does not belong to any playlist!";
        return {};
    }

    PlayInfo playInfo = loadPlayInfo(item, cancel);
    if (playInfo.videos.isEmpty() || cancel.isCancelled())
        return {};

    playInfo.progress = resumePoint(*item);
    return playInfo;
}

// Seeking to its stored fraction would run into the auto-advance that finished it.
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

PlayInfo Playlist::loadPlayInfo(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel) {
    switch (item->type) {
    case PlaylistItem::Pasted: return loadPastedPlayInfo(item);
    case PlaylistItem::Online: return loadOnlinePlayInfo(item, cancel);
    case PlaylistItem::Local:  return loadLocalPlayInfo(item);
    default: return {};
    }
}

PlayInfo Playlist::loadPastedPlayInfo(const QSharedPointer<PlaylistItem> &item) {
    return pastedPlayInfo(item->link);
}

PlayInfo Playlist::pastedPlayInfo(const QString &link) {
    PlayInfo playInfo;
    if (link.contains('|')) {
        QStringList parts = link.split('|');
        playInfo.videos.emplaceBack(parts.takeFirst());
        for (const QString &headerLine : std::as_const(parts)) {
            QStringList keyValue = headerLine.split(": ", Qt::KeepEmptyParts);
            if (keyValue.size() == 2)
                playInfo.headers.insert(keyValue[0].trimmed(), keyValue[1].trimmed());
        }
    } else {
        playInfo.videos.emplaceBack(link);
    }
    return playInfo;
}

namespace {

// Old yt-dlp is the usual reason a site stops working, YouTube first.
QString staleYtdlpHint() {
    const QString version = YtDlp::version();
    const QDate released = QDate::fromString(version.left(10), QStringLiteral("yyyy.MM.dd"));
    if (!released.isValid() || released.daysTo(QDate::currentDate()) <= 60) return {};
    return QStringLiteral("\n\n") + Playlist::tr("yt-dlp %1 is old, and sites change often: update it under "
                                                  "Settings, Player.").arg(version);
}

// A stream mpv can play, or a page yt-dlp finds a video on (when mpv may use yt-dlp). Runs off
// the GUI thread; Settings is read before.
YtDlp::Verdict checkPastedLink(PlayInfo info, bool useYtdlp, const QString &runtime, const CancelToken &cancel) {
    const QUrl url = info.videos.first().url;
    Client client(cancel, false);
    client.setBypassEnabled(false).setMaxBodyBytes(64 * 1024).setTimeout(15000);
    auto headers = info.headers;
    headers.insert(QStringLiteral("Range"), QStringLiteral("bytes=0-2047"));
    const Client::Response response = client.get(url.toString(), headers);
    if (cancel.isCancelled()) return {};

    const QString type = response.header(QStringLiteral("Content-Type")).toLower();
    const QString start = response.body.left(512).trimmed().toLower();
    const bool page = type.contains(QLatin1String("html")) || start.startsWith(QLatin1String("<!doctype html"))
                      || start.startsWith(QLatin1String("<html"));
    if (response.code >= 200 && response.code < 400 && !page) {
        // The probe episodes get, which follows a playlist down to a segment.
        switch (ServerSelector::playability(&client, info)) {
        case ServerSelector::Playability::Playable: return {true, {}};
        case ServerSelector::Playability::Broken: return {false, Playlist::tr("The video at this link doesn't load.")};
        case ServerSelector::Playability::Unknown: return {false, Playlist::tr("The link didn't answer in time.")};
        }
    }
    // A page, or a site that turns plain requests away: yt-dlp may still find the video.
    if (useYtdlp) {
        YtDlp::Verdict verdict = YtDlp::check(url.toString(), info.headers,
                                              YtDlp::runtimeArguments(runtime, YtDlp::version()), cancel);
        if (!verdict.playable && !cancel.isCancelled())
            verdict.error = Playlist::tr("yt-dlp can't play this link: %1").arg(verdict.error) + staleYtdlpHint();
        return verdict;
    }
    if (page)
        return {false, Playlist::tr("That's a web page, not a video. Switch on Use yt-dlp under Settings, Player, "
                                    "to play pages like it.")};
    if (response.code <= 0) return {false, Playlist::tr("Couldn't reach %1.").arg(url.host())};
    return {false, Playlist::tr("The link answered HTTP %1.").arg(response.code)};
}

}

PlayInfo Playlist::loadOnlinePlayInfo(const QSharedPointer<PlaylistItem> &item, const CancelToken &cancel) {
    auto playlist = item->parent();
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
        throw AppException(tr("No servers found for %1").arg(label), tr("Server"));

    std::sort(servers.begin(), servers.end(),
              [](const VideoServer &a, const VideoServer &b) {
                  return a.name < b.name;
              });

    auto result = ServerSelector::findWorkingServer(&client, provider, servers);
    if (!result.found()) {
        // The provider's own message is the only one that explains why.
        if (!result.failure.isEmpty())
            throw AppException(result.failure, result.failureHeader);
        throw AppException(tr("No working server found for %1 (tried %2)")
                               .arg(label).arg(servers.size()), tr("Server"));
    }

    if (cancel.isCancelled()) return {};

    int chosenIndex = result.index;
    QMetaObject::invokeMethod(this, [this, servers, provider, chosenIndex, cancel,
                                     cache = std::move(result.cachedSources)]() mutable {
        if (cancel.isCancelled()) return;
        applyServers(servers, provider, chosenIndex, std::move(cache));
    }, Qt::QueuedConnection);

    return result.playInfo;
}

void Playlist::applyServers(const QList<VideoServer> &servers, ShowProvider *provider,
                                        int chosenIndex, QHash<QString, PlayInfo> cache) {
    m_autoTriedServers.clear();
    const QString winnerName = (chosenIndex >= 0 && chosenIndex < servers.size())
                                   ? servers[chosenIndex].name : QString();
    m_serverListModel.setServers(servers, provider);
    m_serverListModel.setCachedSources(std::move(cache));
    m_serverListModel.setCurrentServer(winnerName);
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
    if (m_prefetch.valid && m_prefetch.itemLink == next->link) return;
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
    if (m_prefetch.valid && m_prefetch.itemLink == next->link) return;
    auto pl = next->parent();
    ShowProvider *provider = pl ? pl->provider() : nullptr;
    if (!provider) return;

    m_prefetchCancel = CancelToken{};
    const QString link = next->link;
    // The link is an opaque token on most providers; the name says which episode it was.
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
                                             idx = result.index,
                                             cache = std::move(result.cachedSources),
                                             info = result.playInfo]() mutable {
                // The run's own token: a newer prefetch has already installed a fresh one.
                if (cancel.isCancelled()) return;
                m_prefetch = Prefetch{ true, QDateTime::currentMSecsSinceEpoch(), link, servers, provider, idx, std::move(cache), info };
                logOk() << "Playlist" << "Prefetched the next episode:" << label;
            }, Qt::QueuedConnection);
        } catch (AppException &e) {
            logWarn() << "Playlist" << "Next-episode prefetch failed:" << e.what();
        } catch (const std::exception &e) {
            logWarn() << "Playlist" << "Next-episode prefetch failed:" << e.what();
        }
    });
}

bool Playlist::tryUsePrefetch(const QSharedPointer<PlaylistItem> &item) {
    if (!m_prefetch.valid || m_prefetch.itemLink != item->link) return false;
    if (QDateTime::currentMSecsSinceEpoch() - m_prefetch.resolvedAtMs > 5 * 60 * 1000) return false;
    if (item->type != PlaylistItem::Online) return false;
    if (m_watcher.isRunning()) return false;

    Prefetch pf = std::move(m_prefetch);
    m_prefetch = {};
    // Its danmaku fetch still belongs to this playback.
    m_cancel.cancel();
    m_cancel = m_prefetchCancel;
    m_prefetchCancel = CancelToken{};
    m_prefetchTimer.stop();

    auto currentItem = m_currentItem.toStrongRef();
    if (currentItem && currentItem != item) saveProgress();

    m_pendingItem.clear();
    m_pendingServerIndex = -1;
    m_bgCacheCancel.cancel();

    applyServers(pf.servers, pf.provider, pf.chosenIndex, std::move(pf.cachedSources));
    m_loadingItem = item;
    ensureMpvProgressConnection();

    PlayInfo playInfo = pf.playInfo;
    playInfo.progress = resumePoint(*item);
    logOk() << "Playlist" << "Using prefetched source for" << item->displayName;
    openResolved(playInfo);
    return true;
}

PlayInfo Playlist::loadLocalPlayInfo(const QSharedPointer<PlaylistItem> &item) {
    if (!QFile::exists(item->link)) {
        logWarn() << "Playlist" << item->link << "does not exist";
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
    PlayInfo playInfo;
    playInfo.videos.emplaceBack(item->link);
    return playInfo;
}

void Playlist::finalizePlayback(const QSharedPointer<PlaylistItem> &item) {
    {
        auto playlist = item->parent();
        if (!playlist) return;

        const int itemRow = item->row();
        playlist->setCurrentIndex(itemRow);
        if (!item->preview) {
            if (playlist->isLocalDir())
                emit localProgressUpdated(item->link, playlist->link, item->progress());
            emit episodeStarted(playlist->link, itemRow, item->name);
            emit progressUpdated(playlist->link, itemRow, item->progress());
        }
        setCurrentItem(item);
        if (!(item->type & PlaylistItem::Online)) m_serverListModel.clear();
        if (const auto started = std::exchange(m_quickPlayPending, {}); started) {
            const auto queued = find(started->link);
            if (queued && queued->season == kQuickPlaySeason && isPlaying(queued->link))
                dropOtherQuickPlayRows(queued->link);
        }
        prefetchNextEpisode();
    }
}

void Playlist::openUrl(QUrl url, bool play) {
    LocalMedia::ParsedUrl parsed = LocalMedia::parse(url);

    if (!parsed.valid) {
        logError() << "Playlist" << "Invalid url:" << parsed.raw;
        AppShell::instance().reportError(tr("Not a file, folder or web address:\n%1").arg(parsed.raw),
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

    // A provider's page url loads as that show, with its servers and tracks.
    QString showLink;
    int episodeIndex = -1;
    if (ShowProvider *provider = ProviderList::forUrl(parsed.url, showLink, episodeIndex)) {
        logInfo() << "Playlist" << "Opening" << parsed.raw << "on" << provider->name();
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
    logInfo() << "Playlist" << "Opening local file" << dirPath;

    auto playlist = find(dirPath);
    if (playlist) {
        if (!pathInfo.isDir())
            playlist->setCurrentIndex(playlist->indexOf(pathInfo.absoluteFilePath()));
    } else {
        playlist = QSharedPointer<PlaylistItem>::create();
        if (LocalMedia::loadFolder(url, playlist, [this](const QString &p) { return m_byLink.contains(p); }, 0, 5)) {
            applyLocalResume(playlist);
            append(playlist);
            logInfo() << "Playlist" << "Loaded folder" << dirPath;
        } else {
            logInfo() << "Playlist" << "Failed to load folder" << dirPath;
            AppShell::instance().reportError(tr("Nothing playable in %1.").arg(dirPath),
                                             tr("Cannot open"));
            playlist = nullptr;
        }
    }

    if (playlist && play) {
        if (auto *mpv = MpvPlayer::instance()) mpv->showText(tr("Playing: %1").arg(urlString));
        tryPlay(playlist);
    }
}

// Checked first, so a link that won't play neither joins the playlist nor replaces the video.
// Only web links: mpv takes other schemes (rtmp, rtsp, udp) as they are.
void Playlist::openRemoteUrl(const QString &urlString, const QUrl &url, bool play) {
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        addPastedUrl(urlString, url, play);
        return;
    }
    if (play) {
        m_linkCheck.cancel();   // a newer link to play wins
        m_linkCheck = CancelToken{};
    }
    const CancelToken cancel = play ? m_linkCheck : m_queueCheck;
    logInfo() << "Playlist" << "Checking" << urlString;
    if (auto *mpv = MpvPlayer::instance()) mpv->showText(tr("Checking %1...").arg(url.host()));
    const bool useYtdlp = Settings::instance().mpvYtdlEnabled();
    const QString runtime = Settings::instance().value(QStringLiteral("ytdlp/jsRuntime")).toString();

    ++m_linkChecks;
    emit isLoadingChanged();
    auto *watcher = new QFutureWatcher<YtDlp::Verdict>(this);
    connect(watcher, &QFutureWatcher<YtDlp::Verdict>::finished, this, [this, watcher, cancel, urlString, url, play] {
        watcher->deleteLater();
        --m_linkChecks;
        emit isLoadingChanged();
        if (cancel.isCancelled()) return;
        const YtDlp::Verdict verdict = watcher->result();
        if (verdict.playable) {
            addPastedUrl(urlString, url, play);
        } else {
            logWarn() << "Playlist" << "Not playable:" << urlString << verdict.error;
            AppShell::instance().reportError(verdict.error, tr("Cannot play link"));
        }
    });
    watcher->setFuture(QtConcurrent::run([info = pastedPlayInfo(urlString), useYtdlp, runtime, cancel] {
        return checkPastedLink(info, useYtdlp, runtime, cancel);
    }));
}

void Playlist::addPastedUrl(const QString &urlString, const QUrl &url, bool play) {
    logInfo() << "Playlist" << "Opening online video" << urlString;

    // What reaches here is a bare media url; grouping by host beats one "Videos" bucket.
    ShowProvider *owner = ProviderList::ownerOfHost(url);
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
    auto playlist = find(path);
    if (!playlist) {
        logError() << "Playlist" << "Untracked path" << path;
        return;
    }
    auto currentItem = m_currentItem.toStrongRef();
    auto currentParent = currentItem ? currentItem->parent() : nullptr;
    bool isCurrentPlaylist = currentParent == playlist;
    QString prevLink = isCurrentPlaylist ? currentItem->link : "";

    if (isCurrentPlaylist) saveProgress(true);

    logInfo() << "Playlist" << "Directory" << path << "has changed";
    deregisterPlaylist(playlist);
    beginResetModel();
    const bool loaded = LocalMedia::loadFolder(QUrl::fromLocalFile(path), playlist,
                                                [this](const QString &p) { return m_byLink.contains(p); });
    if (loaded) applyLocalResume(playlist);
    endResetModel();
    if (loaded) {
        registerPlaylist(playlist);
        if (isCurrentPlaylist) {
            // A rescan must not move the current item while its file exists.
            const int idx = prevLink.isEmpty() ? -1 : playlist->indexOf(prevLink);
            if (idx >= 0) {
                playlist->setCurrentIndex(idx);
                setCurrentItem(playlist->at(idx));
            } else {
                setCurrentItem(playlist->currentItem());
                tryPlay(playlist);
            }
        }
        return;
    }

    logInfo() << "Playlist" << "Failed to reload folder" << playlist->link;
    if (auto *mpv = MpvPlayer::instance()) mpv->pause();
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
    if (!playlist || !playlist->isList() || m_byLink.contains(playlist->link)) return;
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
    return nextList ? nextList->name : QString();
}

int Playlist::currentShowEpisodeCount() const {
    auto cur = m_currentItem.toStrongRef();
    auto parent = cur ? cur->parent() : nullptr;
    return parent ? parent->episodeCount() : 0;
}

// Null for pasted urls and local folders.
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
    // A film, or a show of one episode, goes by its title alone.
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
    return parentItem->count();
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
        auto parent = item->parent();
        if (!parent || parent->currentIndex() == -1) return false;
        return parent->currentIndex() == item->row();
    }
    case IsDeletableRole:
        return (item->count() > 0) || ((item->type & PlaylistItem::Pasted) != 0);
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
    };
}
void Playlist::openResolved(PlayInfo &info) {
    if (auto *mpv = MpvPlayer::instance()) {
        if (m_loadingItem) {
            auto parent = m_loadingItem->parent();
            mpv->setShowKey(parent ? parent->link : m_loadingItem->link);
            mpv->setEpisodeKey(m_loadingItem->link);
        }
        if (m_recoveryProgress >= 0) info.progress = std::exchange(m_recoveryProgress, -1.0);
        mpv->open(info);
    }
}
